#! /usr/bin/env python3

import sys

def replace(s: str):
    return s[s.find('(') + 1: s.find(')')]

print(*map(replace, filter(lambda s: s.strip(), sys.stdin.readlines())), sep='\n')
