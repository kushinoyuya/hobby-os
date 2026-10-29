# make      : build/os.img を作る
# make run  : ビルドして QEMU で起動する
# make clean: ビルド成果物を消す
#
# ディスクイメージ(build/os.img)の中身:
#   セクタ 0              : boot.asm   ステージ1(BIOS が 0x7C00 に読み込む)
#   セクタ 1              : stage2.asm ステージ2(ステージ1が 0x7E00 に読み込む)
#   セクタ 2〜LOAD_SECTORS: カーネル   (ステージ1が 0x8000 に読み込む)

BUILD := build
IMG   := $(BUILD)/os.img

# ステージ1がディスクから読み込むセクタ数(ステージ2 + カーネル)
LOAD_SECTORS := 32
KERNEL_MAX   := $(shell echo $$(( ($(LOAD_SECTORS) - 1) * 512 )))

NASMFLAGS := -f bin -DLOAD_SECTORS=$(LOAD_SECTORS)

# OS の上ではなく「OS そのもの」を作るためのコンパイル設定
#   -m32 -march=i686     : 32ビットの x86 向けに作る
#   -mgeneral-regs-only  : SSE などの拡張命令を使わない(まだ有効にしていないため)
#   -ffreestanding       : 標準ライブラリがない環境向け
#   -fno-pie             : 決まった番地(0x8000)に置く前提で作る
#   -fno-stack-protector : 標準ライブラリに頼るスタック保護機能を使わない
CFLAGS := -m32 -march=i686 -mgeneral-regs-only -ffreestanding -fno-pie \
          -fno-stack-protector -fno-asynchronous-unwind-tables \
          -O2 -Wall -Wextra
#   --no-warn-rwx-segments: 1つの領域に全部置く単純な構成なので、その警告を出さない
LDFLAGS := -m elf_i386 -nostdlib -T kernel/linker.ld --no-warn-rwx-segments

all: $(IMG)

$(IMG): $(BUILD)/boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin
	cat $^ > $@
	truncate -s $$(( (1 + $(LOAD_SECTORS)) * 512 )) $@

$(BUILD)/boot.bin $(BUILD)/stage2.bin: $(BUILD)/%.bin: %.asm print16.inc Makefile | $(BUILD)
	nasm $(NASMFLAGS) $< -o $@

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	objcopy -O binary $< $@
	@size=$$(stat -c %s $@); if [ $$size -gt $(KERNEL_MAX) ]; then \
		echo "カーネル($$size バイト)が大きすぎます。Makefile の LOAD_SECTORS を増やしてください"; \
		rm -f $@; exit 1; fi

$(BUILD)/kernel.elf: $(BUILD)/entry.o $(BUILD)/kernel.o kernel/linker.ld
	ld $(LDFLAGS) -o $@ $(BUILD)/entry.o $(BUILD)/kernel.o

$(BUILD)/entry.o: kernel/entry.asm Makefile | $(BUILD)
	nasm -f elf32 $< -o $@

$(BUILD)/kernel.o: kernel/kernel.c Makefile | $(BUILD)
	gcc $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

run: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG)

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
