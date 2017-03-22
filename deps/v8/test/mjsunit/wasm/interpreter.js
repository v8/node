// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Flags: --wasm-interpret-all --allow-natives-syntax

load('test/mjsunit/wasm/wasm-constants.js');
load('test/mjsunit/wasm/wasm-module-builder.js');

// The stack trace contains file path, only keep "interpreter.js".
let stripPath = s => s.replace(/[^ (]*interpreter\.js/g, 'interpreter.js');

function checkStack(stack, expected_lines) {
  print('stack: ' + stack);
  var lines = stack.split('\n');
  assertEquals(expected_lines.length, lines.length);
  for (var i = 0; i < lines.length; ++i) {
    let test =
        typeof expected_lines[i] == 'string' ? assertEquals : assertMatches;
    test(expected_lines[i], lines[i], 'line ' + i);
  }
}

(function testCallImported() {
  var stack;
  let func = () => stack = new Error('test imported stack').stack;

  var builder = new WasmModuleBuilder();
  builder.addImport('mod', 'func', kSig_v_v);
  builder.addFunction('main', kSig_v_v)
      .addBody([kExprCallFunction, 0])
      .exportFunc();
  var instance = builder.instantiate({mod: {func: func}});
  // Test that this does not mess up internal state by executing it three times.
  for (var i = 0; i < 3; ++i) {
    var interpreted_before = % WasmNumInterpretedCalls(instance);
    instance.exports.main();
    assertEquals(interpreted_before + 1, % WasmNumInterpretedCalls(instance));
    checkStack(stripPath(stack), [
      'Error: test imported stack',                           // -
      /^    at func \(interpreter.js:\d+:28\)$/,              // -
      '    at main (<WASM>[1]+1)',                            // -
      /^    at testCallImported \(interpreter.js:\d+:22\)$/,  // -
      /^    at interpreter.js:\d+:3$/
    ]);
  }
})();

(function testCallImportedWithParameters() {
  var stack;
  var passed_args = [];
  let func1 = (i, j) => (passed_args.push(i, j), 2 * i + j);
  let func2 = (f) => (passed_args.push(f), 8 * f);

  var builder = new WasmModuleBuilder();
  builder.addImport('mod', 'func1', makeSig([kWasmI32, kWasmI32], [kWasmF32]));
  builder.addImport('mod', 'func2', makeSig([kWasmF64], [kWasmI32]));
  builder.addFunction('main', makeSig([kWasmI32, kWasmF64], [kWasmF32]))
      .addBody([
        // call #0 with arg 0 and arg 0 + 1
        kExprGetLocal, 0, kExprGetLocal, 0, kExprI32Const, 1, kExprI32Add,
        kExprCallFunction, 0,
        // call #1 with arg 1
        kExprGetLocal, 1, kExprCallFunction, 1,
        // convert returned value to f32
        kExprF32UConvertI32,
        // add the two values
        kExprF32Add
      ])
      .exportFunc();
  var instance = builder.instantiate({mod: {func1: func1, func2: func2}});
  var interpreted_before = % WasmNumInterpretedCalls(instance);
  var args = [11, 0.3];
  var ret = instance.exports.main(...args);
  assertEquals(interpreted_before + 1, % WasmNumInterpretedCalls(instance));
  var passed_test_args = [...passed_args];
  var expected = func1(args[0], args[0] + 1) + func2(args[1]) | 0;
  assertEquals(expected, ret);
  assertArrayEquals([args[0], args[0] + 1, args[1]], passed_test_args);
})();

(function testTrap() {
  var builder = new WasmModuleBuilder();
  var foo_idx = builder.addFunction('foo', kSig_v_v)
                    .addBody([kExprNop, kExprNop, kExprUnreachable])
                    .index;
  builder.addFunction('main', kSig_v_v)
      .addBody([kExprNop, kExprCallFunction, foo_idx])
      .exportFunc();
  var instance = builder.instantiate();
  // Test that this does not mess up internal state by executing it three times.
  for (var i = 0; i < 3; ++i) {
    var interpreted_before = % WasmNumInterpretedCalls(instance);
    var stack;
    try {
      instance.exports.main();
      assertUnreachable();
    } catch (e) {
      stack = e.stack;
    }
    assertEquals(interpreted_before + 2, % WasmNumInterpretedCalls(instance));
    checkStack(stripPath(stack), [
      'RuntimeError: unreachable',                    // -
      '    at foo (<WASM>[0]+3)',                     // -
      '    at main (<WASM>[1]+2)',                    // -
      /^    at testTrap \(interpreter.js:\d+:24\)$/,  // -
      /^    at interpreter.js:\d+:3$/
    ]);
  }
})();

(function testThrowFromImport() {
  function func() {
    throw new Error('thrown from imported function');
  }
  var builder = new WasmModuleBuilder();
  builder.addImport("mod", "func", kSig_v_v);
  builder.addFunction('main', kSig_v_v)
      .addBody([kExprCallFunction, 0])
      .exportFunc();
  var instance = builder.instantiate({mod: {func: func}});
  // Test that this does not mess up internal state by executing it three times.
  for (var i = 0; i < 3; ++i) {
    var interpreted_before = % WasmNumInterpretedCalls(instance);
    var stack;
    try {
      instance.exports.main();
      assertUnreachable();
    } catch (e) {
      stack = e.stack;
    }
    assertEquals(interpreted_before + 1, % WasmNumInterpretedCalls(instance));
    checkStack(stripPath(stack), [
      'Error: thrown from imported function',                    // -
      /^    at func \(interpreter.js:\d+:11\)$/,                 // -
      '    at main (<WASM>[1]+1)',                               // -
      /^    at testThrowFromImport \(interpreter.js:\d+:24\)$/,  // -
      /^    at interpreter.js:\d+:3$/
    ]);
  }
})();

(function testGlobals() {
  var builder = new WasmModuleBuilder();
  builder.addGlobal(kWasmI32, true);  // 0
  builder.addGlobal(kWasmI64, true);  // 1
  builder.addGlobal(kWasmF32, true);  // 2
  builder.addGlobal(kWasmF64, true);  // 3
  builder.addFunction('get_i32', kSig_i_v)
      .addBody([kExprGetGlobal, 0])
      .exportFunc();
  builder.addFunction('get_i64', kSig_d_v)
      .addBody([kExprGetGlobal, 1, kExprF64SConvertI64])
      .exportFunc();
  builder.addFunction('get_f32', kSig_d_v)
      .addBody([kExprGetGlobal, 2, kExprF64ConvertF32])
      .exportFunc();
  builder.addFunction('get_f64', kSig_d_v)
      .addBody([kExprGetGlobal, 3])
      .exportFunc();
  builder.addFunction('set_i32', kSig_v_i)
      .addBody([kExprGetLocal, 0, kExprSetGlobal, 0])
      .exportFunc();
  builder.addFunction('set_i64', kSig_v_d)
      .addBody([kExprGetLocal, 0, kExprI64SConvertF64, kExprSetGlobal, 1])
      .exportFunc();
  builder.addFunction('set_f32', kSig_v_d)
      .addBody([kExprGetLocal, 0, kExprF32ConvertF64, kExprSetGlobal, 2])
      .exportFunc();
  builder.addFunction('set_f64', kSig_v_d)
      .addBody([kExprGetLocal, 0, kExprSetGlobal, 3])
      .exportFunc();
  var instance = builder.instantiate();
  // Initially, all should be zero.
  assertEquals(0, instance.exports.get_i32());
  assertEquals(0, instance.exports.get_i64());
  assertEquals(0, instance.exports.get_f32());
  assertEquals(0, instance.exports.get_f64());
  // Assign values to all variables.
  var values = [4711, 1<<40 + 1 << 33, 0.3, 12.34567];
  instance.exports.set_i32(values[0]);
  instance.exports.set_i64(values[1]);
  instance.exports.set_f32(values[2]);
  instance.exports.set_f64(values[3]);
  // Now check the values.
  assertEquals(values[0], instance.exports.get_i32());
  assertEquals(values[1], instance.exports.get_i64());
  assertEqualsDelta(values[2], instance.exports.get_f32(), 2**-23);
  assertEquals(values[3], instance.exports.get_f64());
})();

(function testReentrantInterpreter() {
  var stacks;
  var instance;
  function func(i) {
    stacks.push(new Error('reentrant interpreter test #' + i).stack);
    if (i < 2) instance.exports.main(i + 1);
  }

  var builder = new WasmModuleBuilder();
  builder.addImport('mod', 'func', kSig_v_i);
  builder.addFunction('main', kSig_v_i)
      .addBody([kExprGetLocal, 0, kExprCallFunction, 0])
      .exportFunc();
  instance = builder.instantiate({mod: {func: func}});
  // Test that this does not mess up internal state by executing it three times.
  for (var i = 0; i < 3; ++i) {
    var interpreted_before = % WasmNumInterpretedCalls(instance);
    stacks = [];
    instance.exports.main(0);
    assertEquals(interpreted_before + 3, % WasmNumInterpretedCalls(instance));
    assertEquals(3, stacks.length);
    for (var e = 0; e < stacks.length; ++e) {
      expected = ['Error: reentrant interpreter test #' + e];
      expected.push(/^    at func \(interpreter.js:\d+:17\)$/);
      expected.push('    at main (<WASM>[1]+3)');
      for (var k = e; k > 0; --k) {
        expected.push(/^    at func \(interpreter.js:\d+:33\)$/);
        expected.push('    at main (<WASM>[1]+3)');
      }
      expected.push(
          /^    at testReentrantInterpreter \(interpreter.js:\d+:22\)$/);
      expected.push(/    at interpreter.js:\d+:3$/);
      checkStack(stripPath(stacks[e]), expected);
    }
  }
})();
