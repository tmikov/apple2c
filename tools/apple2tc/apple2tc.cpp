/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Disas.h"

#include "apple2tc/support.h"

#include <stdexcept>
#include <tuple>

static std::tuple<std::vector<uint8_t>, uint16_t> loadInputBinary(const char *inputPath) {
  std::vector<uint8_t> binary;
  uint16_t start;
  uint16_t size;

  if (FILE *f = fopen(inputPath, "rb")) {
    binary = readAll<std::vector<uint8_t>>(f);
    fclose(f);
  } else {
    perror(inputPath);
    exit(2);
  }

  if (binary.size() < 4) {
    fprintf(stderr, "%s: missing DOS3.3 file header\n", inputPath);
    exit(3);
  }
  start = binary[0] + binary[1] * 256;
  size = binary[2] + binary[3] * 256;
  if (size > 0x10000 - start || size != binary.size() - 4) {
    fprintf(stderr, "%s: invalid DOS3.3 file header\n", inputPath);
    exit(3);
  }

  binary.erase(binary.begin(), binary.begin() + 4);

  return {std::move(binary), start};
}

static const char *s_appPath;
static void printHelp() {
  fprintf(stderr, "syntax: %s [--asm] [--simple-c] input_file\n", s_appPath);
  fprintf(stderr, "  --asm        Generate asm listing\n");
  fprintf(stderr, "  --simple-c   Generate simple C code\n");
}

int main(int argc, char **argv) {
  enum class Action {
    GenAsm,
    GenSimpleC,
  };
  Action action = Action::GenAsm;
  s_appPath = argc ? argv[0] : "a2tc";

  std::string inputPath;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--asm") == 0) {
      action = Action::GenAsm;
      continue;
    }
    if (strcmp(argv[i], "--simple-c") == 0) {
      action = Action::GenSimpleC;
      continue;
    }
    if (argv[i][0] == '-') {
      printHelp();
      return 1;
    }

    if (inputPath.empty()) {
      inputPath = argv[i];
      continue;
    }

    fprintf(stderr, "Too many arguments\n");
    printHelp();
    return 1;
  }

  if (inputPath.empty()) {
    fprintf(stderr, "Not enough arguments\n");
    printHelp();
    return 1;
  }

  // Load the input binary.
  auto [binary, start] = loadInputBinary(inputPath.c_str());

  try {
    auto dis = std::make_unique<Disas>();
    dis->loadBinary(start, binary.data(), binary.size());
    dis->run(start);
    switch (action) {
    case Action::GenAsm:
      dis->printAsmListing();
      break;
    case Action::GenSimpleC:
      dis->printSimpleC(stdout);
      break;
    }
  } catch (std::exception &ex) {
    fprintf(stderr, "*** FATAL: %s\n", ex.what());
    return 2;
  }

  return 0;
}