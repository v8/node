# Node.js

This is the official [V8](https://developers.google.com/v8/) fork of Node.js with a recent (usually Canary) V8 version. 

Download the executable for Ubuntu from this [build bot](https://build.chromium.org/p/client.v8.fyi/builders/V8%20-%20node.js%20integration). Select a build, then use *Archive link download*.

To check the V8 version in Node, have a look at [v8-version.h](https://github.com/v8/node/blob/vee-eight-lkgr/deps/v8/include/v8-version.h) or run 

```
node -e "console.log(process.versions.v8)"
```

