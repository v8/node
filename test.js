'use strict';

for(var i =0; i< 1E7;i++) {
  if (i % 1E4 == 0) gc();
  new class{};
}
