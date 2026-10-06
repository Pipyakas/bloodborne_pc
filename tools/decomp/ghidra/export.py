"""Export hexadecimal image-offset function entries from the shared project."""
import argparse
from pipeline import run

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('addresses', nargs='+', help='Hex image offsets, with or without 0x')
    a = p.parse_args()
    for address in a.addresses:
        int(address, 16)
    run('export', a.addresses)
