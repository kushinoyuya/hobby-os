# make      : build/boot.img を作る
# make run  : ビルドして QEMU で起動する
# make clean: ビルド成果物を消す

BUILD := build
IMG   := $(BUILD)/boot.img

all: $(IMG)

$(IMG): boot.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD):
	mkdir -p $@

run: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG)

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
