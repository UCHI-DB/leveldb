//
// Created by harper on 12/9/20.
//

#include "vert_block_builder.h"

#include "db/dbformat.h"

#include "table/format.h"

namespace colsm {

VertSectionBuilder::VertSectionBuilder(EncodingType enc_type)
    : num_entry_(0), value_enc_type_(enc_type) {
  Encoding& encoding = string::EncodingFactory::Get(enc_type);
  value_encoder_ = encoding.encoder();
}

void VertSectionBuilder::Open(uint32_t sv) {
  start_value_ = sv;
  num_entry_ = 0;
  key_encoder_.Open();
  seq_encoder_.Open();
  type_encoder_.Open();
  value_encoder_->Open();
}

void VertSectionBuilder::Reset() { num_entry_ = 0; }

void VertSectionBuilder::Add(ParsedInternalKey key, const Slice& value) {
  num_entry_++;

  key_encoder_.Encode((*(uint32_t*)(key.user_key.data()) - start_value_));
  seq_encoder_.Encode(key.sequence);
  type_encoder_.Encode((uint8_t)key.type);
  value_encoder_->Encode(value);
}

uint32_t VertSectionBuilder::EstimateSize() const {
  return 28 + key_encoder_.EstimateSize() + seq_encoder_.EstimateSize() +
         type_encoder_.EstimateSize() + value_encoder_->EstimateSize();
}

void VertSectionBuilder::Close() {
  key_encoder_.Close();
  seq_encoder_.Close();
  type_encoder_.Close();
  value_encoder_->Close();
}

void VertSectionBuilder::Dump(uint8_t* out) {
  uint8_t* pointer = (uint8_t*)out;
  *reinterpret_cast<uint32_t*>(pointer) = num_entry_;
  pointer += 4;
  *reinterpret_cast<uint32_t*>(pointer) = start_value_;
  pointer += 4;

  auto key_size = key_encoder_.EstimateSize();
  auto seq_size = seq_encoder_.EstimateSize();
  auto type_size = type_encoder_.EstimateSize();
  auto value_size = value_encoder_->EstimateSize();

  *((uint32_t*)pointer) = key_size;
  pointer += 4;
  *(pointer++) = BITPACK;
  *((uint32_t*)pointer) = seq_size;
  pointer += 4;
  *(pointer++) = PLAIN;
  *((uint32_t*)pointer) = type_size;
  pointer += 4;
  *(pointer++) = RUNLENGTH;
  *((uint32_t*)pointer) = value_size;
  pointer += 4;
  *(pointer++) = value_enc_type_;

  key_encoder_.Dump(pointer);
  pointer += key_size;
  seq_encoder_.Dump(pointer);
  pointer += seq_size;
  type_encoder_.Dump(pointer);
  pointer += type_size;
  value_encoder_->Dump(pointer);
}

VertBlockBuilder::VertBlockBuilder(const Options* options,
                                   EncodingType value_encoding)
    : BlockBuilder(options),
      value_encoding_(value_encoding),
      section_limit_(options->section_limit),
      current_section_(value_encoding),
      offset_(0) {}

// Assert the keys and values are both int32_t
void VertBlockBuilder::Add(const Slice& key, const Slice& value) {
  // Need to handle the internal key
  ParsedInternalKey internal_key;
  ParseInternalKey(key, &internal_key);

  // write other parts of the internal key
  if (current_section_.NumEntry() == 0) {
    uint32_t intkey =
        *reinterpret_cast<const uint32_t*>(internal_key.user_key.data());
    current_section_.Open(intkey);
  }
  current_section_.Add(internal_key, value);
  if (current_section_.NumEntry() >= section_limit_) {
    DumpSection();
  }
}

void VertBlockBuilder::DumpSection() {
  current_section_.Close();
  meta_.AddSection(offset_, current_section_.StartValue());
  auto section_size = current_section_.EstimateSize();
  offset_ += section_size;

  // Write section to buffer
  auto buffer_end = buffer_.size();
  buffer_.resize(buffer_end + section_size);
  current_section_.Dump(buffer_.data() + buffer_end);
  current_section_.Reset();
}

void VertBlockBuilder::Reset() {
  current_section_.Reset();
  offset_ = 0;
  buffer_.clear();
  meta_.Reset();
}

Slice VertBlockBuilder::Finish() {
  if (current_section_.NumEntry() != 0) {
    DumpSection();
  }
  meta_.Finish();

  auto meta_size = meta_.EstimateSize();

  // Allocate Space for meta
  auto buffer_end = buffer_.size();
  buffer_.resize(buffer_end + meta_size + 8);
  auto pointer = buffer_.data() + buffer_end;
  meta_.Write(pointer);
  pointer += meta_size;
  // Meta size
  *((uint32_t*)pointer) = meta_size;
  pointer += 4;
  // MAGIC
  *((uint32_t*)pointer) = MAGIC;

  return Slice((const char*)buffer_.data(), buffer_.size());
}

size_t VertBlockBuilder::CurrentSizeEstimate() const {
  // sizes of meta
  auto meta_size = meta_.EstimateSize();
  auto section_size = offset_;
  if (current_section_.NumEntry() != 0) {
    // The size of each new section is upper-bounded by two 64-bits
    meta_size += 16;
    section_size += current_section_.EstimateSize();
  }
  // sizes of dumped sections

  return meta_size + section_size + 4;
}

bool VertBlockBuilder::empty() const {
  return offset_ == 0 && current_section_.NumEntry() == 0;
}

}  // namespace colsm