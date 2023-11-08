'use strict';

const common = require('../common');

// This test ensures that Node.js throws an Error when trying to convert a
// large buffer into a string.
// Regression test for https://github.com/nodejs/node/issues/649.

if (!common.enoughTestMem) {
  common.skip('skipped due to memory requirements');
}

const assert = require('assert');
const { Buffer } = require('buffer');

// Find the maximum supported buffer length.
let limit = 1 << 31; // 2GB
while (true) {
  try {
    Buffer(limit);
    limit *= 2;
  } catch (e) {
    break;
  }
}

const message = {
  code: 'ERR_OUT_OF_RANGE',
  name: 'RangeError',
};
assert.throws(() => Buffer(limit).toString('utf8'), message);
assert.throws(() => Buffer.alloc(limit).toString('utf8'), message);
assert.throws(() => Buffer.allocUnsafe(limit).toString('utf8'), message);
assert.throws(() => Buffer.allocUnsafeSlow(limit).toString('utf8'), message);
