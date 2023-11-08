'use strict';
require('../common');

// This test ensures that Node.js throws an Error when trying to convert a
// large buffer into a string.
// Regression test for https://github.com/nodejs/node/issues/649.

const assert = require('assert');
const {
  SlowBuffer,
  constants: {
    MAX_STRING_LENGTH,
  },
} = require('buffer');

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
  code: 'ERR_STRING_TOO_LONG',
  name: 'Error',
};
assert.throws(() => Buffer(limit).toString('utf8'), message);
assert.throws(() => SlowBuffer(limit).toString('utf8'), message);
assert.throws(() => Buffer.alloc(limit).toString('utf8'), message);
assert.throws(() => Buffer.allocUnsafe(limit).toString('utf8'), message);
assert.throws(() => Buffer.allocUnsafeSlow(limit).toString('utf8'), message);
