#!/usr/bin/env python3
"""Assert a launcher's process-written state: file section.key=value [...]."""
import configparser
import sys

state = configparser.ConfigParser()
with open(sys.argv[1]) as stream:
    state.read_file(stream)
for expectation in sys.argv[2:]:
    if '>=' in expectation:
        field, value = expectation.split('>=', 1)
        section, key = field.split('.', 1)
        actual = state[section][key]
        if int(actual) < int(value):
            raise SystemExit(f'{field}: expected >= {value!r}, got {actual!r}')
    else:
        field, value = expectation.split('=', 1)
        section, key = field.split('.', 1)
        actual = state[section][key]
        if actual != value:
            raise SystemExit(f'{field}: expected {value!r}, got {actual!r}')
print('PASS: process state assertions')
