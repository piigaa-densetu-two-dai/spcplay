#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

__sfr __at 0x18 PORT0;
__sfr __at 0x19 PORT1;
__sfr __at 0x1a PORT2;
__sfr __at 0x1b PORT3;
__sfr __at 0x1c RESET;
__sfr __at 0xa9 PPIB;
__sfr __at 0xaa PPIC;

#define RDSLT	0x000c
#define CALSLT	0x001c

#define RG1SAV	0xf3e0
#define EXPTBL	0xfcc1

static uint8_t boot[] = {
	/* ポート0にオリジナルの値が書かれるまで待機 */
	0xe4, 0xf4,		/* in0:	mov a, [0xf4]		0x00  0x01		*/
	0x68, 0x00,		/*	cmp a, #port0		0x02 [0x03]		*/
	0xd0, 0xfa,		/*	bne in0			0x04  0x05		*/

	/* データ復元 */
	0x8f, 0x00, 0x00,	/*	mov [0x00], #byte0	0x06 [0x07] 0x08	*/
	0x8f, 0x00, 0x01,	/*	mov [0x01], #byte1	0x09 [0x0a] 0x0b	*/

	/* 各種レジスタ復元 */
	0x8f, 0x00, 0xfa,	/*	mov [0xfa], #timer0	0x0c [0x0d] 0x0e	*/
	0x8f, 0x00, 0xfb,	/*	mov [0xfb], #timer1	0x0f [0x10] 0x11	*/
	0x8f, 0x00, 0xfc,	/*	mov [0xfc], #timer2	0x12 [0x13] 0x14	*/
	0x8f, 0x00, 0xf1,	/*	mov [0xf1], #control	0x15 [0x16] 0x17	*/ /* タイマー設定より後にする 入力ポート設定済なのでポートクリアフラグは落としておく */
	0x8f, 0x6c, 0xf2,	/*	mov [0xf2], 0x6c	0x18  0x19  0x1a	*/
	0x8f, 0x00, 0xf3,	/*	mov [0xf3], #dspflag	0x1b [0x1c] 0x1d	*/
	0x8f, 0x4c, 0xf2,	/*	mov [0xf2], 0x4c	0x1e  0x1f  0x20	*/
	0x8f, 0x00, 0xf3,	/*	mov [0xf3], #dspkey	0x21 [0x22] 0x23	*/
	0x8f, 0x00, 0xf2,	/*	mov [0xf2], #dspaddr	0x24 [0x25] 0x26	*/

	/* CPUレジスタ復元 */
	0xcd, 0x00,		/*	mov x, #sp		0x27 [0x28]		*/
	0xbd,			/*	mov sp, x		0x29			*/
	0xe8, 0x00,		/*	mov a, #a		0x2a [0x2b]		*/
	0xcd, 0x00,		/*	mov x, #x		0x2c [0x2d]		*/
	0x8d, 0x00,		/*	mov y, #y		0x2e [0x2f]		*/

	/* リターン */
	0x7f			/*	reti			0x30			*/
}; /* 49バイト */

#define BOOTADDR (0xffc0 - sizeof(boot)) /* 0xffc0-0xffffはIPL ROM領域 */

#define BOOT_PORT0	0x03
#define BOOT_BYTE0	0x07
#define BOOT_BYTE1	0x0a
#define BOOT_TIMER0	0x0d
#define BOOT_TIMER1	0x10
#define BOOT_TIMER2	0x13
#define BOOT_CONTROL	0x16
#define BOOT_DSPFLAG	0x1c
#define BOOT_DSPKEY	0x22
#define BOOT_DSPADDR	0x25
#define BOOT_SP		0x28
#define BOOT_A		0x2b
#define BOOT_X		0x2d
#define BOOT_Y		0x2f

#if 0
static inline void spc_setaddr(uint8_t count, uint16_t addr)
{
	PORT1 = 1;
	PORT2 = addr & 0xff;
	PORT3 = addr >> 8;
	PORT0 = count;
}

static inline void spc_setdata(uint8_t count, uint8_t value)
{
	PORT1 = value;
	PORT0 = count;
}

static inline void spc_wait(uint8_t count) /* 非同期処理できるようにwaitは別関数にしておく */
{
	while (PORT0 != count);
}
#else /* インラインが使えないっぽいからこちらで回避 */
#define spc_setaddr(c, a)	do { PORT1 = 1; PORT2 = (a) & 0xff; PORT3 = (a) >> 8; PORT0 = (c); } while (0)
#define spc_setdata(c, v)	do { PORT1 = (v); PORT0 = (c); } while (0)
#define spc_wait(c)		do { while (PORT0 != (c)); } while (0)
#endif

/* 不具合回避用 */
static void vdp1wr(uint8_t value) __z88dk_fastcall __naked
{
__asm
	push	af
	push	bc
	push	de
	push	ix
	push	iy
	push	hl

	di

	/* RDSLTの引数設定 */
	ld	a, (EXPTBL)
	ld	hl, 0x0007	/* 書込み用VDPポートが書かれている場所 */

	/* スロットコール */
	ld	iy, (EXPTBL - 1)
	ld	ix, RDSLT
	call	CALSLT

	/* VDPポート1に書込み */
	inc	a		/* 戻り値(0x88か0x98の筈)に+1 */
	ld	c, a
	pop	hl
	ld	a, l		/* lにvalueが格納されている */
	out	(c), a

	pop	iy
	pop	ix
	pop	de
	pop	bc
	pop	af

	ret
__endasm
}

int main(int argc, char *argv[])
{
	int fd = -1;
	uint8_t buf[4096];
	uint16_t pc;
	uint8_t psw, sp;
	char *ptr_track = NULL;
	char *ptr_game = NULL;
	char *ptr_dumper = NULL;
	char *ptr_artist = NULL;
	char *ptr_publisher = NULL;
	char *ptr_year = NULL;
	ssize_t xid6len;
	uint8_t *ptr;
	uint8_t count;
	uint16_t i, j;
	uint8_t port0, port1, port2, port3;

	printf("SPC Player v0.5\n\n");

	if (argc != 2) {
		fprintf(stderr, "Usage: spcplay [FILE]\n");
		return 1;
	}

	/* VDPの割り込み止めて割り込みでリセットがかかる不具合を回避 */
	vdp1wr(*(uint8_t *)RG1SAV & 0b11011111);
	vdp1wr(0b10000001);

	/*
	 * リセット
	 */
	RESET = 0;
	for (i = 1; i; i++) { /* タイムアウト50ミリ秒以上 高速機種に注意 */
		if ((PORT0 == 0xaa) && (PORT1 == 0xbb)) {
			break;
		}
	}
	if (i == 0) {
		fprintf(stderr, "Error: SHVC-SOUND not found.\n");
		goto error;
	}

	/*
	 * ファイルオープン
	 */
#if 0	/* -subtype=msxdos2でエラーしてしまう */
	struct stat st;
	if (stat(argv[1], &st)) {
		fprintf(stderr, "Error: no such file.\n");
		goto error;
	}
	if (st.st_size < 0x10200) {
		fprintf(stderr, "Error: invalid file.\n");
		goto error;
	}
#endif
	if ((fd = open(argv[1], O_RDONLY, 0644)) < 0) {
		fprintf(stderr, "Error: open failed.\n");
		goto error;
	}

	/*
	 * ファイルヘッダー処理
	 */
	if (read(fd, buf, 256) != 256) {
		fprintf(stderr, "Error: read failed.\n");
		goto error;
	}
	if (memcmp(buf, "SNES-SPC700 Sound File Data", 27)) {
		fprintf(stderr, "Error: invalid file.\n");
		goto error;
	}
	pc = (buf[0x25] << 8) | buf[0x26]; /* プログラムカウンタ */
	boot[BOOT_A] = buf[0x27]; /* Aレジスタ */
	boot[BOOT_X] = buf[0x28]; /* Xレジスタ */
	boot[BOOT_Y] = buf[0x29]; /* Yレジスタ */
	psw = buf[0x2a]; /* PSWレジスタ */
	sp = buf[0x2b]; /* スタックポインタ */
	if (sp < 0x03) {
		sp = 0x03;
	}
	boot[BOOT_SP] = sp - 0x03;

	/*
	 * ID666タグを処理
	 */
	if (buf[0x23] == 0x1a) { /* タグ有り */
		ptr_track = (char *)&buf[0x2e];
		ptr_game = (char *)&buf[0x4e];
		ptr_dumper = (char *)&buf[0x6e];
		/* テキストフォーマットとバイナリーフォーマットの判定は困難 */
		if (!buf[0xb0]) {
			ptr_artist = (char *)&buf[0xb1]; /* 恐らくテキスト */
		} else if (isdigit(buf[0xac]) && isdigit(buf[0xad]) && isdigit(buf[0xae]) && isdigit(buf[0xaf]) && isdigit(buf[0xb0])) {
			ptr_artist = (char *)&buf[0xb1]; /* 恐らくテキスト */
		} else {
			ptr_artist = (char *)&buf[0xb0]; /* 恐らくバイナリー */
		}
	}

	/*
	 * XID6タグを処理
	 */
	if (lseek(fd, 0x10200, SEEK_SET) != 0x10200) {
		fprintf(stderr, "Error: lseek failed.\n");
		goto error;
	}
	ptr = &buf[256];
	xid6len = read(fd, ptr, sizeof(buf) - 256);
	if ((8 <= xid6len) && (memcmp(ptr, "xid6", 4) == 0)) { /* タグ有り */
		if (xid6len - 8 < *(uint32_t *)&ptr[4]) {
			xid6len = 0; /* 長さが不正 */
		} else {
			xid6len = *(uint32_t *)&ptr[4];
			ptr += 8;
		}
	} else {
		xid6len = 0;
	}
	while (4 <= xid6len) {
		if (ptr[1] == 0x00) { /* データ型 */
			if (ptr[0] == 0x14) {
				ptr_year = (char *)&ptr[2];
			}
			ptr += 4;
			xid6len -= 4;
		} else if (ptr[1] == 0x01) { /* 文字列型 */
			if (xid6len <= 4) {
				break; /* 文字列の実体が無い */
			}
			uint16_t len = (*(uint16_t *)&ptr[2] + 3) & ~3; /* 4バイト単位に切り上げ */
			if (256 < len) {
				break; /* 長さが不正 */
			}
			switch (ptr[0]) {
				case 0x01:
					ptr_track = (char *)&ptr[4];
					break;
				case 0x02:
					ptr_game = (char *)&ptr[4];
					break;
				case 0x03:
					ptr_artist = (char *)&ptr[4];
					break;
				case 0x04:
					ptr_dumper = (char *)&ptr[4];
					break;
				case 0x13:
					ptr_publisher = (char *)&ptr[4];
					break;
				default:
					break;
			}
			ptr += (4 + len);
			xid6len -= (4 + len);
		} else if (ptr[1] == 0x04) { /* 整数型 */
			ptr += 8;
			xid6len -= 8;
		} else { /* 型が不正 */
			break;
		}
	}

	/*
	 * タグ情報の表示
	 */
#define IS_EXTENDED(ptr) (&buf[256] <= (ptr))
	if (ptr_game) {
		printf(IS_EXTENDED(ptr_game) ? "Game  : %.255s" : "Game  : %.32s", ptr_game);
		if (ptr_publisher) {
			printf(" - %.255s", ptr_publisher);
		}
		if (ptr_year) {
			printf(" [%u]", *(uint16_t *)ptr_year);
		}
		printf("\n");
	}
	if (ptr_track) {
		printf(IS_EXTENDED(ptr_track) ? "Track : %.255s\n" : "Track : %.32s\n", ptr_track);
	}
	if (ptr_artist) {
		printf(IS_EXTENDED(ptr_artist) ? "Artist: %.255s\n" : "Artist: %.32s\n", ptr_artist);
	}
	if (ptr_dumper) {
		printf(IS_EXTENDED(ptr_dumper) ? "Dumper: %.255s\n" : "Dumper: %.16s\n", ptr_dumper);
	}
	printf("\n");

	/*
	 * DSPレジスタ RAMデータ処理より先にやっておく
	 */
	printf("Loading DSP registers...\n");
	if (lseek(fd, 0x10100, SEEK_SET) != 0x10100) {
		fprintf(stderr, "Error: lseek failed.\n");
		goto error;
	}
	if (read(fd, buf, 128) != 128) {
		fprintf(stderr, "Error: read failed.\n");
		goto error;
	}
	count = 0xcc; /* 初期値 */
	for (i = 0; i < 128; i++) {
		if (i == 0x4c) { /* DSPキーオン */
			boot[BOOT_DSPKEY] = buf[i];
			buf[i] = 0x00; /* ミュート */
		} else if (i == 0x6c) { /* DSPフラグ */
			boot[BOOT_DSPFLAG] = buf[i];
			buf[i] = 0x60; /* ミュート メモリエコーOFF */
		}
		spc_setaddr(count, 0x00f2); /* countは0xccか3にしかなり得ない */
		spc_wait(count);
		count = 0x00;
		spc_setdata(count, i); /* DSPアドレス */
		spc_wait(count);
		count++; /* 1になる */
		spc_setdata(count, buf[i]); /* DSPデータ */
		spc_wait(count);
		count += 2; /* 3になる */
	}

	/*
	 * RAMデータ
	 */
	printf("Loading RAM data...\n");

	/* ページ0部分 */
	if (lseek(fd, 0x100, SEEK_SET) != 0x100) {
		fprintf(stderr, "Error: lseek failed.\n");
		goto error;
	}
	if (read(fd, buf, 4096) != 4096) {
		fprintf(stderr, "Error: read failed.\n");
		goto error;
	}
	count = (count + 2) | 1; /* +2以上で0は不可という条件なのでこうする */
	spc_setaddr(count, 0x0002);
	spc_wait(count);
	count = 0x00;
	for (i = 0x0002; i < 0x00f0; i++) {
		spc_setdata(count, buf[i]);
		spc_wait(count);
		count++;
	}
	port0 = buf[0xf4]; /* 0xf4-0xf7はSPC700側から設定できないのでMSX側から設定する */
	port1 = buf[0xf5];
	port2 = buf[0xf6];
	port3 = buf[0xf7];
	boot[BOOT_PORT0] = port0; /* PORT0の復元を開始トリガーとする */
	boot[BOOT_BYTE0] = buf[0x00]; /* 最初の2バイトはIPL ROMで使用するのでブートプログラムで復元する */
	boot[BOOT_BYTE1] = buf[0x01];
	boot[BOOT_CONTROL] = buf[0xf1] & 0b11001111; /* ポートクリアフラグを落とす */
	boot[BOOT_DSPADDR] = buf[0xf2];
	boot[BOOT_TIMER0] = buf[0xfa];
	boot[BOOT_TIMER1] = buf[0xfb];
	boot[BOOT_TIMER2] = buf[0xfc];

	/* ページ1部分(スタック領域) */
	count = (count + 2) | 1; /* +2以上で0は不可という条件なのでこうする */
	spc_setaddr(count, 0x0100);
	spc_wait(count);
	count = 0x00;
	for (i = 0; i < 256; i++) {
		if (sp - 2 == i) { /* retiでPSWが復元されるようにする */
			buf[256 + i] = psw;
		} else if (sp - 1 == i) { /* retiでSPCプログラムが起動されるようにする */
			buf[256 + i] = pc >> 8;
		} else if (sp == i) {
			buf[256 + i] = pc & 0xff;
		}
		spc_setdata(count, buf[256 + i]);
		spc_wait(count);
		count++;
	}

	/* 残りの部分 ここは非同期書込みして少し高速化 */
	spc_setdata(count, buf[512]);
	for (i = 513; i < 4096; i++) {
		spc_wait(count);
		count++;
		spc_setdata(count, buf[i]);
	}
	for (i = 0; i < 15; i++) { /* 最初の4096バイトは転送済なのでその分引く */
		if (read(fd, buf, 4096) != 4096) {
			fprintf(stderr, "Error: read failed.\n");
			goto error;
		}
		for (j = 0; j < 4096; j++) {
			spc_wait(count);
			count++;
			spc_setdata(count, buf[j]);
		}
	}
	spc_wait(count);

	/*
	 * ファイルクローズ
	 */
	close(fd);

	/*
	 * 起動
	 */
	printf("Booting...\n");
	count = (count + 2) | 1; /* +2以上で0は不可という条件なのでこうする */
	spc_setaddr(count, BOOTADDR);
	spc_wait(count);
	count = 0x00;
	for (i = 0; i < sizeof(boot); i++) {
		spc_setdata(count, boot[i]);
		spc_wait(count);
		count++;
	}
	count = (count + 2) | 1; /* +2以上で0は不可という条件なのでこうする */
	if (count == port0) { /* PORT0の復元を開始トリガーとしているのでPORT0と一致した場合が値をずらす */
		count = (count + 1) | 1;
	}
	PORT3 = BOOTADDR >> 8;
	PORT2 = BOOTADDR & 0xff;
	PORT1 = 0x00;
	PORT0 = count;
	spc_wait(count);
	PORT3 = port3;
	PORT2 = port2;
	PORT1 = port1;
	PORT0 = port0; /* PORT0の復元が開始トリガーなのでPORT0を最後に設定 */

	/* ESCが押されるのを待つ */
	printf("\nPress ESC to exit.\n");
	PPIC = (PPIC & 0b11110000) | 0x7;
	while (PPIB & 0x4);

	RESET = 0;
	vdp1wr(*(uint8_t *)RG1SAV); /* 元に戻す */
	vdp1wr(0b10000001);

	return 0;

error:
	if (fd >= 0) {
		close(fd);
	}
	RESET = 0;
	vdp1wr(*(uint8_t *)RG1SAV); /* 元に戻す */
	vdp1wr(0b10000001);

	return 1;
}
