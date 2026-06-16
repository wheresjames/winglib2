function failHere() {
  throw new Error("stack smoke failure");
}

failHere();
