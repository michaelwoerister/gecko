// |reftest| skip -- Promise.allSettled is not supported
// Copyright (C) 2019 Leo Balter. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-promise.allsettled
description: Promise.allSettled property descriptor
info: |
  ES Section 17

  Every other data property described in clauses 18 through 26 and in Annex
  B.2 has the attributes { [[Writable]]: true, [[Enumerable]]: false,
  [[Configurable]]: true } unless otherwise specified.
includes: [propertyHelper.js]
features: [Promise.allSettled]
---*/

verifyProperty(Promise, 'allSettled', {
  configurable: true,
  writable: true,
  enumerable: false,
});

reportCompare(0, 0);
