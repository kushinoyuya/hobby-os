# make      : build/os.img を作る
# make run  : ビルドして QEMU で起動する
# make clean: ビルド成果物を消す
#
# ディスクイメージの中身:
#   セクタ 0              : boot.asm(ステージ1、512バイト)
#   セクタ 1〜STAGE2_SECTORS: stage2.asm(ステージ2)

BUILD := build
IMG   := $(BUILD)/os.img

# ステージ2の大きさ(セクタ数)。boot.asm と stage2.asm の両方に同じ値を渡す
STAGE2_SECTORS := 32
NASMFLAGS := -f bin -DSTAGE2_SECTORS=$(STAGE2_SECTORS)

all: $(IMG)

$(IMG): $(BUILD)/boot.bin $(BUILD)/stage2.bin
	cat $^ > $@

$(BUILD)/%.bin: %.asm print16.inc Makefile | $(BUILD)
	nasm $(NASMFLAGS) $< -o $@

$(BUILD):
	mkdir -p $@

run: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG)

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
