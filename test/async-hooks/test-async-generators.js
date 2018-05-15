'use strict';
const common = require('../common');

// This test checks the structure of the async resource tree
// when async generator is being looped with "for async...of"

// TODO: Update this test when promises are optimized out again

const assert = require('assert');
const initHooks = require('./init-hooks');
const { checkInvocations } = require('./hook-checks');

const util = require('util');

const sleep = util.promisify(setTimeout);
const timeout = common.platformTimeout(10);

const hooks = initHooks({});
hooks.enable();

function checkAsyncTreeLink(node, parentId, invocations) {
  assert.strictEqual(node.triggerAsyncId, parentId);
  checkInvocations(node, invocations, 'when process exits');
}

process.on('exit', function onexit() {
  hooks.disable();
  hooks.sanityCheck('PROMISE');

  const as = hooks.activitiesOfTypes('PROMISE');
  assert.strictEqual(as.length, 17);

  checkAsyncTreeLink(as[0], 1, { init: 1, promiseResolve: 1 });
  checkAsyncTreeLink(as[1], 1, { init: 1, promiseResolve: 1 });
  checkAsyncTreeLink(as[2], 1, { init: 1, promiseResolve: 1 });
  checkAsyncTreeLink(as[3], as[1].uid, { init: 1, promiseResolve: 2 });
  checkAsyncTreeLink(as[4], as[3].uid,
                     { init: 1, promiseResolve: 1, before: 1, after: 1 });
  checkAsyncTreeLink(as[5], as[0].uid, { init: 1, promiseResolve: 2 });
  checkAsyncTreeLink(as[6], as[5].uid,
                     { init: 1, promiseResolve: 1, before: 1, after: 1 });
  checkAsyncTreeLink(as[7], as[2].uid,
                     { init: 1, promiseResolve: 1, before: 1, after: 1 });
  checkAsyncTreeLink(as[8], as[1].uid,
                     { init: 1, promiseResolve: 1, before: 1, after: 1 });
  checkAsyncTreeLink(as[9], as[1].uid, { init: 1, promiseResolve: 1 });
  checkAsyncTreeLink(as[10], as[9].uid,
                     { init: 1, promiseResolve: 1, before: 1, after: 1 });
  checkAsyncTreeLink(as[11], as[6].uid, { init: 1, promiseResolve: 1 });
  checkAsyncTreeLink(as[12], as[11].uid, { init: 1, promiseResolve: 1 });
  checkAsyncTreeLink(as[13], as[12].uid,
                     { init: 1, promiseResolve: 1, before: 1, after: 1 });
  checkAsyncTreeLink(as[14], as[0].uid, { init: 1, promiseResolve: 2 });
  checkAsyncTreeLink(as[15], as[14].uid,
                     { init: 1, promiseResolve: 1, before: 1, after: 1 });
  checkAsyncTreeLink(as[16], as[11].uid,
                     { init: 1, promiseResolve: 1, before: 1, after: 1 });
});

async function runAsyncGen() {
  for await (const line of asyncGen()) {
    line;
  }
}

async function* asyncGen() {
  await sleep(timeout);
  yield 'result';
}

runAsyncGen();
