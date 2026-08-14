from pathlib import Path
import re
import sys

REG8 = {'B': 0, 'C': 1, 'D': 2, 'E': 3, 'H': 4, 'L': 5, 'M': 6, 'A': 7}
REG16 = {'B': 0, 'D': 1, 'H': 2, 'SP': 3}


def value(token, labels):
    token = token.strip().upper()
    for operator in ('+', '-'):
        if operator in token[1:]:
            left, right = token.split(operator, 1)
            base = value(left, labels)
            offset = value(right, labels)
            return base + offset if operator == '+' else base - offset
    if token in labels:
        return labels[token]
    if len(token) == 3 and token[0] == "'" and token[2] == "'":
        return ord(token[1])
    if token.startswith('$'):
        return int(token[1:], 16)
    if token.endswith('H'):
        return int(token[:-1], 16)
    return int(token, 10)


def split_args(text):
    args = []
    current = []
    quoted = False
    for char in text:
        if char == "'":
            quoted = not quoted
        if char == ',' and not quoted:
            args.append(''.join(current).strip())
            current = []
        else:
            current.append(char)
    if current:
        args.append(''.join(current).strip())
    return args


def data_bytes(args, labels, emit):
    output = []
    for arg in args:
        arg = arg.strip()
        if len(arg) >= 2 and arg[0] == "'" and arg[-1] == "'":
            output.extend(ord(char) for char in arg[1:-1])
        else:
            output.append(value(arg, labels) & 0xFF)
    return output if emit else [0] * len(output)


def instruction(op, args, labels, emit):
    op = op.upper()
    args = split_args(args) if args else []
    immediate8 = {'MVI': 2, 'CPI': 1}
    branches = {'JMP': 0xC3, 'JZ': 0xCA, 'JNZ': 0xC2, 'CALL': 0xCD, 'JC': 0xDA, 'JNC': 0xD2}
    returns = {'RET': 0xC9, 'RZ': 0xC8, 'RNZ': 0xC0}
    address16 = {'STA': 0x32, 'LDA': 0x3A, 'SHLD': 0x22, 'LHLD': 0x2A}
    noArgs = {'XCHG': 0xEB}
    simple = {
        'INX': {'B': 0x03, 'D': 0x13, 'H': 0x23},
        'DCR': {'B': 0x05, 'D': 0x15, 'A': 0x3D, 'C': 0x0D},
        'LDAX': {'D': 0x1A},
        'STAX': {'D': 0x12},
        'PUSH': {'D': 0xD5, 'B': 0xC5, 'H': 0xE5},
        'POP': {'D': 0xD1, 'B': 0xC1, 'H': 0xE1},
        'DAD': {'B': 0x09, 'D': 0x19, 'H': 0x29, 'SP': 0x39},
    }
    if op in returns:
        return [returns[op]]
    if op in noArgs:
        return [noArgs[op]]
    if op in address16:
        addr = value(args[0], labels) if emit else 0
        return [address16[op], addr & 0xFF, addr >> 8] if emit else [0, 0, 0]
    if op in branches:
        target = value(args[0], labels) if emit else 0
        return [branches[op], target & 0xFF, target >> 8] if emit else [0, 0, 0]
    if op == 'LXI':
        reg, imm = args
        opcode = 0x01 + 0x10 * REG16[reg.upper()]
        number = value(imm, labels) if emit else 0
        return [opcode, number & 0xFF, number >> 8] if emit else [0, 0, 0]
    if op == 'MVI':
        reg, imm = args
        number = value(imm, labels) if emit else 0
        return [0x06 + 8 * REG8[reg.upper()], number & 0xFF] if emit else [0, 0]
    if op == 'CPI':
        number = value(args[0], labels) if emit else 0
        return [0xFE, number & 0xFF] if emit else [0, 0]
    if op == 'ADI':
        number = value(args[0], labels) if emit else 0
        return [0xC6, number & 0xFF] if emit else [0, 0]
    if op == 'SUI':
        number = value(args[0], labels) if emit else 0
        return [0xD6, number & 0xFF] if emit else [0, 0]
    if op == 'ADD':
        return [0x80 + REG8[args[0].upper()]]
    if op in simple:
        return [simple[op][args[0].upper()]]
    if op == 'MOV':
        dst, src = args
        return [0x40 + 8 * REG8[dst.upper()] + REG8[src.upper()]]
    raise ValueError(f'unsupported instruction: {op} {args}')


def parse(path):
    rows = []
    labels = {}
    address = 0x100
    equates = {}
    for raw in path.read_text().splitlines():
        line = raw.split(';', 1)[0].strip()
        if not line:
            continue
        label = None
        if ':' in line:
            label, line = line.split(':', 1)
            label = label.strip().upper()
            labels[label] = address
            line = line.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        directive = parts[0].upper()
        rest = parts[1] if len(parts) > 1 else ''
        if directive not in {'ORG', 'DB', 'DS', 'END', 'EQU'} and rest.upper().startswith('EQU'):
            label = directive
            directive, rest = 'EQU', rest[3:].strip()
        if directive == 'ORG':
            address = value(rest, {**equates, **labels})
            rows.append((address, 'ORG', rest))
            continue
        if directive == 'EQU':
            if label is None:
                raise ValueError('EQU requires a label')
            equates[label] = value(rest, {**equates, **labels})
            continue
        rows.append((address, directive, rest))
        if directive == 'DB':
            address += len(data_bytes(split_args(rest), {**equates, **labels}, False))
        elif directive == 'DS':
            address += value(rest, {**equates, **labels})
        elif directive == 'END':
            break
        else:
            address += len(instruction(directive, rest, {**equates, **labels}, False))
    labels.update(equates)
    return rows, labels


def assemble(source, output):
    rows, labels = parse(source)
    image = bytearray()
    origin = 0x100
    address = origin
    for row_address, op, rest in rows:
        if op == 'ORG':
            address = value(rest, labels)
            if not image:
                origin = address
            continue
        if op == 'END':
            break
        if address < origin:
            raise ValueError('address moved before origin')
        if len(image) < address - origin:
            image.extend(b'\0' * (address - origin - len(image)))
        if op == 'DB':
            encoded = data_bytes(split_args(rest), labels, True)
        elif op == 'DS':
            encoded = [0] * value(rest, labels)
        else:
            encoded = instruction(op, rest, labels, True)
        image.extend(encoded)
        address += len(encoded)
    output.write_bytes(image)
    print(f'Wrote {output} ({len(image)} bytes) at origin 0x{origin:04X}')


if __name__ == '__main__':
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('CPM22/MINICCP.ASM')
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else Path('MINICCP.COM')
    assemble(source, output)
