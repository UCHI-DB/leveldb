//
// Created by harper on 12/9/20.
//

#include "vert_block.h"

#include <immintrin.h>

#include "byteutils.h"
#include "sboost.h"
#include "unpacker.h"

namespace colsm {

using namespace encoding;

int eq_packed(const uint8_t* data, uint32_t num_entry, uint8_t bitwidth,
              uint32_t target) {
  uint32_t mask = (1 << bitwidth) - 1;
  uint32_t begin = 0;
  uint32_t end = num_entry - 1;
  while (begin <= end) {
    auto current = (begin + end + 1) / 2;

    auto bits = current * bitwidth;
    auto index = bits >> 3;
    auto offset = bits & 0x7;

    auto extracted = (*(uint32_t*)(data + index) >> offset) & mask;

    if (extracted == target) {
      return current;
    }
    if (extracted > target) {
      end = current - 1;
    } else {
      begin = current + 1;
    }
  }
  return -1;
}

int section_packed(uint8_t* data, uint32_t num_entry, uint8_t bitwidth,
                   uint32_t target) {
  uint32_t mask = (1 << bitwidth) - 1;
  uint32_t begin = 0;
  uint32_t end = num_entry - 1;
  while (begin < end) {
    auto current = (begin + end + 1) / 2;

    auto bits = current * bitwidth;
    auto index = bits >> 3;
    auto offset = bits & 0x7;

    auto extracted = (*(uint32_t*)(data + index) >> offset) & mask;

    if (extracted <= target) {
      begin = current;
    } else {
      end = current - 1;
    }
  }
  return begin;
}

VertBlockMeta::VertBlockMeta()
    : num_section_(0), start_min_(0), start_bitwidth_(0), starts_(NULL) {}

VertBlockMeta::~VertBlockMeta() {}

void VertBlockMeta::Reset() {
  num_section_ = 0;
  start_min_ = 0;
  start_bitwidth_ = 0;
  starts_ = nullptr;
  starts_plain_.clear();
  offsets_.clear();
}

uint32_t VertBlockMeta::SectionOffset(uint32_t sec_index) {
  return offsets_[sec_index];
}

void VertBlockMeta::AddSection(uint64_t offset, int32_t start_value) {
  num_section_++;
  if (starts_plain_.empty()) {
    start_min_ = start_value;
  }
  starts_plain_.push_back(start_value - start_min_);
  offsets_.push_back(offset);
}

uint64_t VertBlockMeta::Search(int32_t value) {
  auto target = value - start_min_;
  return offsets_[section_packed(starts_, num_section_, start_bitwidth_,
                                 target)];
}

uint32_t VertBlockMeta::Read(const uint8_t* in) {
  auto pointer = in;
  num_section_ = *reinterpret_cast<const uint32_t*>(pointer);
  pointer += 4;
  offsets_.resize(num_section_);
  memcpy(offsets_.data(), pointer, num_section_ * 8);
  pointer += num_section_ * 8;
  start_min_ = *reinterpret_cast<const int32_t*>(pointer);
  pointer += 4;
  start_bitwidth_ = *(pointer++);

  starts_ = (uint8_t*)pointer;

  return pointer - in;
}

void VertBlockMeta::Finish() {
  start_bitwidth_ = 32 - _lzcnt_u32(starts_plain_[starts_plain_.size() - 1]);
}

uint32_t VertBlockMeta::EstimateSize() const {
  return 9 + num_section_ * 8 + BitPackSize();
}

void VertBlockMeta::Write(uint8_t* out) {
  auto pointer = out;
  *reinterpret_cast<uint32_t*>(pointer) = num_section_;
  pointer += 4;
  memcpy(pointer, offsets_.data(), 8 * num_section_);
  pointer += 8 * num_section_;
  *reinterpret_cast<int32_t*>(pointer) = start_min_;
  pointer += 4;

  *reinterpret_cast<uint8_t*>(pointer++) = start_bitwidth_;
  sboost::byteutils::bitpack(starts_plain_.data(), num_section_,
                             start_bitwidth_, (uint8_t*)pointer);

  //  memcpy(pointer, starts_, (start_bitwidth_ * num_section_ + 7) >> 3);
}

VertSection::VertSection() : num_entry_(0) {}

void VertSection::Read(const uint8_t* in) {
  auto pointer = in;
  num_entry_ = *reinterpret_cast<const uint32_t*>(pointer);
  pointer += 4;
  start_value_ = *reinterpret_cast<const int32_t*>(pointer);
  pointer += 4;

  auto key_size = *((uint32_t*)pointer);
  pointer += 4;
  EncodingType key_enc = (EncodingType) * (pointer++);
  auto seq_size = *((uint32_t*)pointer);
  pointer += 4;
  EncodingType seq_enc = (EncodingType) * (pointer++);
  auto type_size = *((uint32_t*)pointer);
  pointer += 4;
  EncodingType type_enc = (EncodingType) * (pointer++);
  auto value_size = *((uint32_t*)pointer);
  pointer += 4;
  EncodingType value_enc = (EncodingType) * (pointer++);

  // Read data about key encoding
  bit_width_ = *(pointer);
  key_data_ = pointer + 1;
  assert(key_enc == BITPACK);
  key_decoder_ = u32::EncodingFactory::Get(BITPACK).decoder();
  key_decoder_->Attach(pointer);
  pointer += key_size;

  seq_decoder_ = u64::EncodingFactory::Get(seq_enc).decoder();
  seq_decoder_->Attach(pointer);
  pointer += seq_size;

  type_decoder_ = u8::EncodingFactory::Get(type_enc).decoder();
  type_decoder_->Attach(pointer);
  pointer += type_size;

  value_decoder_ = string::EncodingFactory::Get(value_enc).decoder();
  value_decoder_->Attach(pointer);
}

int32_t VertSection::Find(int32_t target) {
  //  sboost::SortedBitpack sbp(bit_width_, target - start_value_);
  //  return sbp.equal(keys_data_, num_entry_);
  return eq_packed(key_data_, num_entry_, bit_width_, target - start_value_);
}

int32_t VertSection::FindStart(int32_t target) {
  sboost::SortedBitpack sbp(bit_width_, target - start_value_);
  return sbp.geq(key_data_, num_entry_);
}

VertBlockCore::VertBlockCore(const BlockContents& data)
    : raw_data_((uint8_t*)data.data.data()),
      size_(data.data.size()),
      owned_(data.heap_allocated) {
  meta_.Read(raw_data_);
  content_data_ = raw_data_ + meta_.EstimateSize();
}

VertBlockCore::~VertBlockCore() {
  if (owned_) {
    delete[] raw_data_;
  }
}

class VertBlockCore::VIter : public Iterator {
 private:
  const Comparator* const comparator_;
  VertBlockMeta& meta_;
  const uint8_t* data_pointer_;

  uint32_t section_index_ = -1;
  VertSection section_;
  uint32_t entry_index_ = -1;

  char key_buffer_[12];
  Slice key_;
  Slice value_;

  Status status_;

  void ReadSection(int sec_index) {
    section_index_ = sec_index;
    section_.Read(data_pointer_ + meta_.SectionOffset(section_index_));
  }

  void ComposeKeyValue() {
      *((int32_t*)key_buffer_) = section_.StartValue()+section_.KeyDecoder()->DecodeU32();
      auto seq = section_.SeqDecoder()->DecodeU64();
      auto type = section_.TypeDecoder()->DecodeU8();
      *((uint64_t*)(key_buffer_+4)) = (seq << 8) + type;
      value_ = section_.ValueDecoder()->Decode();
  }

 public:
  VIter(const Comparator* comparator, VertBlockMeta& meta, const uint8_t* data)
      : comparator_(comparator), meta_(meta),
        data_pointer_(data),key_(key_buffer_,12) {
    ReadSection(0);
  }

  void Seek(const Slice& target) override {
    // Scan through blocks
    int32_t target_key = *reinterpret_cast<const int32_t*>(target.data());

    auto new_section_index = meta_.Search(target_key);
    if (new_section_index != section_index_) {
      ReadSection(new_section_index);
    }

    entry_index_ = section_.Find(target_key);
    if (entry_index_ == -1) {
      // Not found
      status_ = Status::NotFound(target);
    } else {
      // Seek to the position, extract the keys and values

      section_.KeyDecoder()->Skip(entry_index_);
      section_.SeqDecoder()->Skip(entry_index_);
      section_.TypeDecoder()->Skip(entry_index_);
      section_.ValueDecoder()->Skip(entry_index_);
      ComposeKeyValue();
    }
  }

  void SeekToFirst() override {
    // Not supported
    status_ = Status::NotSupported(Slice("SeekToFirst Not Supported"));
  }

  void SeekToLast() override {
    // Not supported
    status_ = Status::NotSupported(Slice("SeekToLast Not Supported"));
  }

  void Next() override {
    entry_index_++;
    if (entry_index_ >= section_.NumEntry()) {
      if (section_index_ < meta_.NumSection() - 1) {
        ReadSection(section_index_ + 1);
        entry_index_ = 0;
      } else {
        section_index_++;
        // No more element
        return;
      }
    }
    ComposeKeyValue();
  }

  void Prev() override {
    // Not supported
    status_ = Status::NotSupported(Slice("Prev Not Supported"));
  }

  bool Valid() const override {
    return entry_index_ < section_.NumEntry() ||
           section_index_ < meta_.NumSection() - 1;
  }

  Slice key() const override { return key_; }

  Slice value() const override { return value_; }

  Status status() const override { return status_; }
};

Iterator* VertBlockCore::NewIterator(const Comparator* comparator) {
  return new VIter(comparator, meta_, content_data_);
}

}  // namespace colsm