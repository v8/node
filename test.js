const async_hooks = require("async_hooks");
const fs = require("fs");

async_hooks
  .createHook({
    init: (asyncId, type, triggerAsyncId, resource) => {
      fs.writeSync(1, `${triggerAsyncId} => ${asyncId}\n`);
    }
  })
  .enable();

async function main() {
  console.log("hello");
  await null;
  console.log("hello");
}

main();
