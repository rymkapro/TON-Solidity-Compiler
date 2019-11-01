# Sol2TVM Compiler

Port of the Solidity smart-contract [compiler](https://github.com/ethereum/solidity) generating TVM bytecode for TON blockchain. Please refer to upstream README.md for information on the language itself.

## Build and Install
Original Instructions about how to build and install the Solidity compiler can be found in the [Solidity documentation](https://solidity.readthedocs.io/en/latest/installing-solidity.html#building-from-source).

### Ubuntu Linux:
```shell
git clone git@github.com:tonlabs/TON-Solidity-Compiler.git
cd compiler 
sh ./scripts/install_deps.sh
mkdir build
cd build
cmake .. -DUSE_CVC4=OFF -DUSE_Z3=OFF -DTESTS=OFF -DCMAKE_BUILD_TYPE=Debug
make
```

### Windows 10:
Install Visual Studio 2017, Git bash, cmake.
(might be outdated)
```shell
git clone https://github.com/tonlabs/TON-Solidity-Compiler
cd compiler
scripts\install_deps.bat
mkdir build
cd build
cmake .. -DUSE_CVC4=OFF -DUSE_Z3=OFF -DTESTS=OFF -DCMAKE_BUILD_TYPE=Debug -G "Visual Studio 15 2017 Win64"
cmake --build . --config Release
```

## Usage

[Described in the samples repository](https://github.com/tonlabs/samples/tree/master/solidity)

All relevant binaries are delivered within TON Labs Node SE at https://ton.dev/.

