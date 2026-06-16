# C++ Examples

Each example directory can be built either as part of the Winglib2 source tree
or copied to another directory and built against an installed Winglib2 package.

```sh
cmake -S embedded -B embedded-build -DCMAKE_PREFIX_PATH=/path/to/winglib2/install
cmake --build embedded-build
```

`CMAKE_PREFIX_PATH` is only needed when Winglib2 is installed somewhere CMake
does not search by default.
