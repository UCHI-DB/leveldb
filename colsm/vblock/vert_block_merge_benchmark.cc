//
// Created by Hao Jiang on 12/2/20.
//
#include <benchmark/benchmark.h>
#include <iostream>
#include <memory>

#include "leveldb/comparator.h"

#include "table/block.h"
#include "table/block_builder.h"
#include "table/format.h"

#include "colsm/comparators.h"
#include "sortmerge_iterator.h"
#include "vert_block.h"
#include "vert_block_builder.h"

using namespace std;
using namespace leveldb;
using namespace colsm;

bool binary_sorter(int a, int b) { return memcmp(&a, &b, 4) < 0; }

int value_len = 128;
uint32_t batch_size = 240000;
BlockContents prepareBlock(std::vector<int>& keys) {
  uint32_t num_entry = keys.size();
  char key_buffer[12];
  char value_buffer[value_len];
  Slice key((const char*)key_buffer, 12);
  Slice value(value_buffer, value_len);

  Options option;
  option.comparator = leveldb::BytewiseComparator();
  BlockBuilder builder((const Options*)&option);

  for (auto i = 0; i < num_entry; ++i) {
      *((uint32_t*)key_buffer) = keys[i];
    builder.Add(key, value);
  }
  auto result = builder.Finish();
  char* copied = new char[result.size()];
  memcpy(copied, result.data(), result.size());
  Slice heap(copied, result.size());
  return BlockContents{heap, true, false};
}

BlockContents prepareVBlock(std::vector<int>& keys,
                            EncodingType encoding) {
  auto comparator = colsm::intComparator();
  uint32_t num_entry = keys.size();
  char key_buffer[12];
  char value_buffer[value_len];
  Slice key((const char*)&key_buffer, 12);
  Slice value(value_buffer, value_len);

  Options option;
  option.comparator = comparator.get();
  VertBlockBuilder builder((const Options*)&option,encoding);

  for (auto i = 0; i < num_entry; ++i) {
      *((uint32_t*)key_buffer)= keys[i];
    builder.Add(key, value);
  }
  auto result = builder.Finish();
  char* copied = new char[result.size()];
  memcpy(copied, result.data(), result.size());
  Slice heap(copied, result.size());
  return BlockContents{heap, true, false};
}

void BlockMergeWithNoOverlap(benchmark::State& state) {
  vector<int32_t> key1;
  vector<int32_t> key2;
  for (int i = 0; i < batch_size; ++i) {
    //        key1.push_back(i);
    //        key2.push_back(i+1000000);
    key1.push_back(i * 2);
    key2.push_back(i * 2 + 1);
  }
  sort(key1.begin(), key1.end(), binary_sorter);
  sort(key2.begin(), key2.end(), binary_sorter);
  auto content1 = prepareBlock(key1);
  auto content2 = prepareBlock(key2);

  for (auto _ : state) {
    Block block1(content1);
    Block block2(content2);

    Options option;
    option.comparator = leveldb::BytewiseComparator();
    BlockBuilder builder((const Options*)&option);

    auto ite1 = block1.NewIterator(leveldb::BytewiseComparator());
    auto ite2 = block2.NewIterator(leveldb::BytewiseComparator());
    ite1->SeekToFirst();
    ite2->SeekToFirst();
    auto ite =
        colsm::sortMergeIterator(leveldb::BytewiseComparator(), ite1, ite2);

    while (ite->Valid()) {
      builder.Add(ite->key(), ite->value());
      ite->Next();
    }
    builder.Finish();
    //        std::cout << counter << "\n";
  }

  delete content1.data.data();
  delete content2.data.data();
}

void VBlockMergeWithNoOverlap(benchmark::State& state) {
  vector<int32_t> key1;
  vector<int32_t> key2;
  auto comparator = colsm::intComparator();
  for (int i = 0; i < batch_size; ++i) {
    key1.push_back(i * 2);
    key2.push_back(i * 2 + 1);
  }

  auto content1 = prepareVBlock(key1, EncodingType::LENGTH);
  auto content2 = prepareVBlock(key2, EncodingType::LENGTH);

  for (auto _ : state) {
    VertBlockCore block1(content1);
    VertBlockCore block2(content2);

    Options option;
    option.comparator = comparator.get();
    VertBlockBuilder builder((const Options*)&option,LENGTH);

    auto ite1 = block1.NewIterator(comparator.get());
    auto ite2 = block2.NewIterator(comparator.get());
    ite1->SeekToFirst();
    ite2->SeekToFirst();
    auto ite = colsm::sortMergeIterator(comparator.get(), ite1, ite2);
    while (ite->Valid()) {
      builder.Add(ite->key(), ite->value());
      ite->Next();
    }
    builder.Finish();
  }

  delete content1.data.data();
  delete content2.data.data();
}

BENCHMARK(BlockMergeWithNoOverlap);
BENCHMARK(VBlockMergeWithNoOverlap);
