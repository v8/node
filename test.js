function fib(x) {
  if (x < 2) return 1;
  return fib(x - 1) + fib(x - 2);
}

function g() {

}

console.log(fib(3));
