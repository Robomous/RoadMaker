/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Direct unit for roadmaker/xodr/raw_xml.hpp (audit 2026-07 gap B4): the
// Preserved-tier passthrough (docs/design/m3a/01 §5). RawXml captures unmodeled
// attributes and children IN DOCUMENT ORDER, and the writer re-emits them in
// that order — so operator== must be order-sensitive, and empty() must be true
// only when there is genuinely nothing to re-emit.

#include "roadmaker/xodr/raw_xml.hpp"

#include <gtest/gtest.h>

using roadmaker::RawXml;

TEST(RawXml, DefaultConstructedIsEmpty) {
  EXPECT_TRUE(RawXml{}.empty());
}

TEST(RawXml, AttributesAloneMakeItNonEmpty) {
  RawXml raw;
  raw.attributes.emplace_back("code", "vendor:x");
  EXPECT_FALSE(raw.empty());
}

TEST(RawXml, ChildrenAloneMakeItNonEmpty) {
  RawXml raw;
  raw.children.push_back("<skeleton/>");
  EXPECT_FALSE(raw.empty());
}

TEST(RawXml, EqualContentsCompareEqual) {
  RawXml a;
  a.attributes.emplace_back("code", "vendor:x");
  a.attributes.emplace_back("value", "1");
  a.children.push_back("<userData/>");
  RawXml b = a;
  EXPECT_TRUE(a == b);
}

TEST(RawXml, AttributeOrderIsSignificant) {
  // Document-order contract: the same attributes in a different order would
  // re-emit different bytes, so they are NOT the same preserved payload.
  RawXml a;
  a.attributes.emplace_back("code", "vendor:x");
  a.attributes.emplace_back("value", "1");
  RawXml b;
  b.attributes.emplace_back("value", "1");
  b.attributes.emplace_back("code", "vendor:x");
  EXPECT_FALSE(a == b);
}

TEST(RawXml, ChildOrderIsSignificant) {
  RawXml a;
  a.children.push_back("<material/>");
  a.children.push_back("<markings/>");
  RawXml b;
  b.children.push_back("<markings/>");
  b.children.push_back("<material/>");
  EXPECT_FALSE(a == b);
}

TEST(RawXml, DifferingAttributeValueCompareUnequal) {
  RawXml a;
  a.attributes.emplace_back("code", "vendor:x");
  RawXml b;
  b.attributes.emplace_back("code", "vendor:y");
  EXPECT_FALSE(a == b);
}
