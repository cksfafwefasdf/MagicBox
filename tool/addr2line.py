import re
import sys

# 这个脚本用于内核奔溃时进行栈回溯
# 通过奔溃的函数地址找到相应的函数名

MAP_FILE = "../build/kernel/kernel.map"

def lookup_address(target_addr):
    # 转换为整数方便比较处理
    target_addr = int(target_addr, 16)
    found_func = "Unknown"
    closest_addr = 0

    # 正则表达式：匹配 0xc000xxxx 和对应的函数名
    pattern = re.compile(r"0x([0-9a-fA-F]{8,16})\s+([a-zA-Z_][a-zA-Z0-9_]*)")

    with open(MAP_FILE, 'r') as f:
        for line in f:
            match = pattern.search(line)
            if match:
                addr = int(match.group(1), 16)
                name = match.group(2)
                
                # 寻找小于目标地址且最接近的那个符号
                if addr <= target_addr > closest_addr:
                    closest_addr = addr
                    found_func = name

    if found_func != "Unknown":
        offset = target_addr - closest_addr
        print(f"Address 0x{target_addr:x} is in <{found_func}+0x{offset:x}>")
    else:
        print(f"Address 0x{target_addr:x} not found in map.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 addr2line.py <hex_address>")
    else:
        lookup_address(sys.argv[1])