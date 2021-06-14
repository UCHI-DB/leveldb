//
// Created by harper on 12/9/20.
//

#include "vert_block.h"

#include <immintrin.h>

#include "byteutils.h"
#include "sboost.h"
#include "unpacker.h"

namespace colsm {

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

uint32_t VertBlockMeta::Read(const char* in) {
  const char* pointer = in;
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

void VertBlockMeta::Write(char* out) {
  char* pointer = out;
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

VertSection::VertSection()
    : num_entry_(0), estimated_size_(0), reading_(true) {}

VertSection::VertSection(const Encodings& enc) : VertSection() {
  reading_ = false;
  encoding_enum_ = enc;
  value_encoding_ = &EncodingFactory::Get(enc);
  value_encoder_ = value_encoding_->encoder();
}

VertSection::~VertSection() {
  if (reading_) delete value_decoder_;
}

void VertSection::Add(int32_t key, const Slice& value) {
  num_entry_++;
  keys_plain_.push_back(key - start_value_);

  value_encoder_->Encode(value);
}

uint32_t VertSection::EstimateSize() {
  if (estimated_size_ != 0) {
    return estimated_size_;
  }
  num_entry_ = keys_plain_.size();
  bit_width_ = 32 - _lzcnt_u32(keys_plain_[num_entry_ - 1]);
  estimated_size_ = 10 + BitPackSize() + value_encoder_->EstimateSize();
  return estimated_size_;
}

void VertSection::Close() { value_encoder_->Close(); }

void VertSection::Dump(char* out) {
  auto pointer = out;
  *reinterpret_cast<uint32_t*>(pointer) = num_entry_;
  pointer += 4;
  *reinterpret_cast<int32_t*>(pointer) = start_value_;
  pointer += 4;
  *reinterpret_cast<uint8_t*>(pointer++) = bit_width_;
  sboost::byteutils::bitpack(keys_plain_.data(), keys_plain_.size(), bit_width_,
                             (uint8_t*)pointer);
  pointer += BitPackSize();

  keys_plain_.clear();
  // Write encoding type
  *(pointer++) = encoding_enum_;
  value_encoder_->Dump(reinterpret_cast<uint8_t*>(pointer));
  delete value_encoder_;
}

void VertSection::Read(const char* in) {
  auto pointer = in;
  num_entry_ = *reinterpret_cast<const uint32_t*>(pointer);
  pointer += 4;
  start_value_ = *reinterpret_cast<const int32_t*>(pointer);
  pointer += 4;
  bit_width_ = *reinterpret_cast<const uint8_t*>(pointer++);
  keys_data_ = reinterpret_cast<const uint8_t*>(pointer);
  pointer += BitPackSize();

  // Read encoding type
  auto enc_type = static_cast<Encodings>(*(pointer++));
  value_encoding_ = &EncodingFactory::Get(enc_type);
  value_decoder_ = value_encoding_->decoder();

  value_decoder_->Attach(reinterpret_cast<const uint8_t*>(pointer));
  // Recompute estimated size
  // TODO Temporarily ignoring value size here as we do not store it
  estimated_size_ = 9 + BitPackSize();
}

int32_t VertSection::Find(int32_t target) {
  //  sboost::SortedBitpack sbp(bit_width_, target - start_value_);
  //  return sbp.equal(keys_data_, num_entry_);
  return eq_packed(keys_data_, num_entry_, bit_width_, target - start_value_);
}

int32_t VertSection::FindStart(int32_t target) {
  sboost::SortedBitpack sbp(bit_width_, target - start_value_);
  return sbp.geq(keys_data_, num_entry_);
}

VertBlockCore::VertBlockCore(const BlockContents& data)
    : raw_data_(data.data.data()),
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
  const char* data_pointer_;

  uint32_t section_index_ = -1;
  VertSection section_;

  uint32_t entry_index_ = -1;
  // To support m256i assignment, make sure the variable is aligned
  alignas(64) __m256i unpacked_;
  uint32_t group_index_ = -1;
  uint32_t group_offset_ = 8;

  sboost::Unpacker* unpacker_;

  int intkey_;
  Slice key_;
  Slice value_;

  Status status_;

  void ReadSection(int sec_index) {
    section_index_ = sec_index;
    section_.Read(data_pointer_ + meta_.SectionOffset(section_index_));
    entry_index_ = -1;
    group_index_ = -1;
    group_offset_ = 8;
  }

  void LoadGroup() {
    unpacker_ = sboost::unpackers[section_.BitWidth()];
    auto group_start = section_.KeysData() + group_index_ * section_.BitWidth();
    auto unpacked = unpacker_->unpack(group_start);
    unpacked_ = unpacked;
  }

  void LoadEntry() {
    auto entry = (unpacked_[group_offset_ / 2] >> (32 * (group_offset_ & 1))) &
                 (uint32_t)-1;
    intkey_ = entry + section_.StartValue();
  }

 public:
  VIter(const Comparator* comparator, VertBlockMeta& meta, const char* data)
      : comparator_(comparator),
        key_((const char*)&intkey_, 4),
        meta_(meta),
        data_pointer_(data) {
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
      // TODO Switch to a reader and maintain current unpacking location
      // Unpack 8 entries at a time, read bit_width_ bytes at a time
      group_index_ = entry_index_ >> 3;
      group_offset_ = entry_index_ & 0x7;
      LoadGroup();
      LoadEntry();

      // Sequential skip to value
      auto decoder = section_.ValueDecoder();
      decoder->Skip(entry_index_);
      value_ = decoder->Decode();
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
        group_index_ = -1;
        group_offset_ = 8;
      } else {
        section_index_++;
        // No more element
        return;
      }
    }

    group_offset_++;
    if (group_offset_ >= 8) {
      group_index_++;
      group_offset_ = 0;
      LoadGroup();
    }
    LoadEntry();

    auto decoder = section_.ValueDecoder();
    value_ = decoder->Decode();
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