# make      : build/os.img を作る
# make run  : ビルドして QEMU で起動する
# make run-ping: Linux の ping と nc が届く構成で QEMU を起動する
# make test-tcp: TCP の自動テストを実行する
# make clean: ビルド成果物を消す
#
# ディスクイメージ(build/os.img)の中身:
#   セクタ 0              : boot.asm   ステージ1(BIOS が 0x7C00 に読み込む)
#   セクタ 1              : stage2.asm ステージ2(ステージ1が 0x7E00 に読み込む)
#   セクタ 2〜LOAD_SECTORS: カーネル   (ステージ1が 0x8000 に読み込む)

BUILD := build
IMG   := $(BUILD)/os.img

# ステージ1がディスクから読み込むセクタ数(ステージ2 + カーネル)。256 セクタ = 128KB
LOAD_SECTORS := 256
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

# kernel/ の中の .c と .asm を全部カーネルに入れる(ファイルを増やしても書き足さなくてよい)
KERNEL_OBJS := $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(wildcard kernel/*.c)) \
               $(patsubst kernel/%.asm,$(BUILD)/kernel/%.asm.o,$(wildcard kernel/*.asm))

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	ld $(LDFLAGS) -o $@ $(KERNEL_OBJS)

# -MMD -MP: どの .h を読み込んだかを .d ファイルに記録し、.h を変えたときも作り直す
$(BUILD)/kernel/%.o: kernel/%.c Makefile | $(BUILD)/kernel
	gcc $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/kernel/%.asm.o: kernel/%.asm Makefile | $(BUILD)/kernel
	nasm -f elf32 $< -o $@

-include $(KERNEL_OBJS:.o=.d)

$(BUILD) $(BUILD)/kernel:
	mkdir -p $@

# QEMU に付けるネットワークカード
#   -netdev user      : QEMU の中だけの仮想ネットワーク(10.0.2.0/24、ゲートウェイ 10.0.2.2)
#   -device rtl8139   : そこにつながる RTL8139 を1枚挿す
#   -object filter-dump: 流れたフレームを build/net.pcap に記録する
#                       (tcpdump -r build/net.pcap -e -n や Wireshark で見られる)
#   hostfwd           : この PC の 127.0.0.1:7777 への接続を、自作OSのポート 7 へ転送する
QEMU_NET := -netdev user,id=net0,hostfwd=tcp:127.0.0.1:7777-:7 -device rtl8139,netdev=net0 \
            -object filter-dump,id=dump0,netdev=net0,file=$(BUILD)/net.pcap

run: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG) $(QEMU_NET)

# 本物の Linux の ping を自作OSへ届ける実験室(user モードは外からの ping を通さないため)
run-ping: $(IMG)
	bash tools/ping-lab.sh

# TCP の自動テスト(Python が相手役になり、わざと順番を変えたり壊したりしたセグメントを送る)
test-tcp: $(IMG)
	python3 tools/tcp-test.py

# 本物の Linux の nc とつないで、大きなデータが壊れずに往復するか確かめる
test-tcp-linux: $(IMG)
	bash tools/tcp-linux-test.sh

clean:
	rm -rf $(BUILD)

.PHONY: all run run-ping test-tcp test-tcp-linux clean
