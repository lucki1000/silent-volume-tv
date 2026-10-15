#!/usr/bin/env python3
"""Compile the actual sketch parser: echoed output cannot clear the profile."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sketch = (root / "BLEHIDBridge/BLEHIDBridge.ino").read_text()
start = sketch.index("class SerialCommandLine {")
end = sketch.index("\n};", start) + 3
parser = sketch[start:end]
code = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
''' + parser + r'''
static std::vector<char> decode(const std::string& bytes) {
    SerialCommandLine parser;
    std::vector<char> commands;
    for (char byte : bytes) {
        char command;
        if (parser.feed(byte, command)) commands.push_back(command);
    }
    return commands;
}
int main() {
    assert(decode("s\np\r\nK\r?\n") == std::vector<char>({'s', 'p', 'K', '?'}));
    assert(decode("p").empty()); // No action before a complete line arrives.
    assert(decode("up\nsp\n p\np \n").empty());
    assert(decode("p=clear learned bridge profile\r\nESP32 dual-role BLE HID bridge\n").empty());
    assert(decode("HTTP volume: enabled=yes\nRemote: ready=yes\n").empty());
    assert(decode(std::string(100000, 'p') + "\np\n") == std::vector<char>({'p'}));
    SerialCommandLine split;
    char command = 0;
    assert(!split.feed('p', command) && command == 0);
    assert(!split.feed('x', command) && command == 0);
    assert(!split.feed('\n', command) && command == 0);
    assert(!split.feed('s', command));
    assert(split.feed('\r', command) && command == 's');
    assert(!split.feed('\n', command));
}
'''
with tempfile.TemporaryDirectory(prefix="serial-command-") as directory:
    path = Path(directory)
    source, binary = path / "test.cpp", path / "test"
    source.write_text(code)
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Serial command parser: explicit lines, echo, noise and fragmented input passed")
