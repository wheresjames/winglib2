function rejectHere() {
  return Promise.reject(new Error("promise smoke failure"));
}

await rejectHere();
