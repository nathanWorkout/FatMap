#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sec;
    uint8_t  sec_per_clus;
    uint16_t rsvd_sec_cnt;
    uint8_t  num_fats;
    uint16_t root_ent_cnt;
    uint16_t tot_sec16;
    uint8_t  media;
    uint16_t fat_sz16;
    uint16_t sec_per_trk;
    uint16_t num_heads;
    uint32_t hidd_sec;
    uint32_t tot_sec32;
    uint32_t fat_sz32;
    uint16_t ext_flags;
    uint16_t fs_ver;
    uint32_t root_clus;
} BPB;

enum {
    C_NORMAL = 1,
    C_CYAN, C_YELLOW, C_GRAY, C_SELECTED, C_GREEN, C_MAGENTA, C_RED, C_BLUE,
};

void init_colors() {
    start_color();
    use_default_colors();
    init_pair(C_NORMAL,   COLOR_WHITE,   -1);
    init_pair(C_CYAN,     COLOR_CYAN,    -1);
    init_pair(C_YELLOW,   COLOR_YELLOW,  -1);
    init_pair(C_GRAY,     8,             -1);
    init_pair(C_SELECTED, COLOR_BLACK,   COLOR_CYAN);
    init_pair(C_GREEN,    COLOR_GREEN,   -1);
    init_pair(C_MAGENTA,  COLOR_MAGENTA, -1);
    init_pair(C_RED,      COLOR_RED,     -1);
    init_pair(C_BLUE,     COLOR_BLUE,    -1);
}

int panel_colors[] = { C_CYAN, C_GREEN, C_YELLOW, C_MAGENTA };

static uint32_t *load_fat(FILE *f, BPB *bpb) {
    uint32_t fat_offset = (uint32_t)bpb->rsvd_sec_cnt * bpb->bytes_per_sec;
    uint32_t fat_size   = bpb->fat_sz32 * bpb->bytes_per_sec;
    uint32_t *fat = malloc(fat_size);
    if (!fat) return NULL;
    fseek(f, fat_offset, SEEK_SET);
    if (fread(fat, 1, fat_size, f) < fat_size) { free(fat); return NULL; }
    return fat;
}

static int find_file_in_root(FILE *f, BPB *bpb, const char *name83,
                              uint32_t *out_cluster, uint32_t *out_size) {
    uint32_t bps        = bpb->bytes_per_sec;
    uint32_t spc        = bpb->sec_per_clus;
    uint32_t data_start = ((uint32_t)bpb->rsvd_sec_cnt + bpb->num_fats * bpb->fat_sz32) * bps;
    uint32_t root_off   = data_start + (bpb->root_clus - 2) * spc * bps;
    uint8_t dir[512];
    fseek(f, root_off, SEEK_SET);
    fread(dir, 1, 512, f);
    for (int e = 0; e < 512; e += 32) {
        if (dir[e] == 0x00 || dir[e] == 0xE5 || dir[e+11] == 0x0F) continue;
        if (memcmp(&dir[e], name83, 11) == 0) {
            uint16_t hi = *(uint16_t *)&dir[e + 0x14];
            uint16_t lo = *(uint16_t *)&dir[e + 0x1A];
            *out_cluster = ((uint32_t)hi << 16) | lo;
            *out_size    = *(uint32_t *)&dir[e + 0x1C];
            return 1;
        }
    }
    return 0;
}

static void to_83(const char *input, char out[12]) {
    memset(out, ' ', 11);
    out[11] = '\0';
    const char *dot = strchr(input, '.');
    int name_len = dot ? (int)(dot - input) : (int)strlen(input);
    if (name_len > 8) name_len = 8;
    for (int i = 0; i < name_len; i++) out[i] = (char)toupper((unsigned char)input[i]);
    if (dot) {
        int ext_len = (int)strlen(dot + 1);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++) out[8+i] = (char)toupper((unsigned char)dot[1+i]);
    }
}

void draw_header(const char *filename, int cols) {
    attron(COLOR_PAIR(C_GRAY));
    mvhline(0, 0, '-', cols);
    mvhline(2, 0, '-', cols);
    mvvline(1, 0, '|', 1);
    mvvline(1, cols - 1, '|', 1);
    attroff(COLOR_PAIR(C_GRAY));
    attron(COLOR_PAIR(C_CYAN) | A_BOLD);
    mvprintw(1, 2, "fatmap");
    attroff(COLOR_PAIR(C_CYAN) | A_BOLD);
    attron(COLOR_PAIR(C_NORMAL) | A_BOLD);
    printw("  v0.4");
    attroff(COLOR_PAIR(C_NORMAL) | A_BOLD);
    attron(COLOR_PAIR(C_GRAY));
    printw("  -  ");
    attroff(COLOR_PAIR(C_GRAY));
    attron(COLOR_PAIR(C_NORMAL));
    printw("%s", filename);
    attroff(COLOR_PAIR(C_NORMAL));
    attron(COLOR_PAIR(C_GRAY));
    mvprintw(1, cols - 62, "arrows/j/k scroll   :open  :--check  :--file  :--map   q quit");
    attroff(COLOR_PAIR(C_GRAY));
}

WINDOW *make_panel(int h, int w, int y, int x, const char *title, int active, int color) {
    WINDOW *win = newwin(h, w, y, x);
    int c = active ? color : C_GRAY;
    wattron(win, COLOR_PAIR(c) | (active ? A_BOLD : 0));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(c) | (active ? A_BOLD : 0));
    int tx = (w - (int)strlen(title)) / 2;
    if (tx < 1) tx = 1;
    wattron(win, COLOR_PAIR(active ? C_SELECTED : color) | A_BOLD);
    mvwprintw(win, 0, tx, "%s", title);
    wattroff(win, COLOR_PAIR(active ? C_SELECTED : color) | A_BOLD);
    return win;
}

typedef struct { const char *label; uint32_t value; int hex; int offset; } BPBField;

void draw_bpb(WINDOW *win, BPB *bpb) {
    BPBField fields[] = {
        { "Bytes Per Sector", bpb->bytes_per_sec, 0, 0x0B },
        { "Sectors Per Cluster",  bpb->sec_per_clus,  0, 0x0D },
        { "Reserved Sectors", bpb->rsvd_sec_cnt,  0, 0x0E },
        { "Num FATs",         bpb->num_fats,       0, 0x10 },
        { "Root Entry Count", bpb->root_ent_cnt,   0, 0x11 },
        { "Total Sectors",    bpb->tot_sec32,      1, 0x20 },
        { "FAT Size",         bpb->fat_sz32,       1, 0x24 },
        { "Root Cluster",     bpb->root_clus,      1, 0x2C },
    };
    int n = sizeof(fields) / sizeof(fields[0]);
    int max_h, max_w; getmaxyx(win, max_h, max_w); (void)max_w;
    for (int i = 0; i < n && i + 2 < max_h - 1; i++) {
        wattron(win, COLOR_PAIR(C_GRAY));    mvwprintw(win, i+2, 2,  "+0x%02X",   fields[i].offset); wattroff(win, COLOR_PAIR(C_GRAY));
        wattron(win, COLOR_PAIR(C_CYAN));    mvwprintw(win, i+2, 9,  "%-20s",     fields[i].label);  wattroff(win, COLOR_PAIR(C_CYAN));
        wattron(win, COLOR_PAIR(C_YELLOW) | A_BOLD);
        if (fields[i].hex) mvwprintw(win, i+2, 30, "0x%08X", fields[i].value);
        else               mvwprintw(win, i+2, 30, "%u",      fields[i].value);
        wattroff(win, COLOR_PAIR(C_YELLOW) | A_BOLD);
    }
}

void draw_hex(WINDOW *win, BPB *bpb, const char *filename, int scroll) {
    int max_h, max_w; getmaxyx(win, max_h, max_w); (void)bpb;
    uint8_t sector[512];
    FILE *f = fopen(filename, "rb");
    if (!f) { mvwprintw(win, 2, 2, "Erreur ouverture fichier"); return; }
    size_t rd = fread(sector, 1, 512, f); fclose(f);
    if (rd < 512) { mvwprintw(win, 2, 2, "Lecture incomplete"); return; }

    wattron(win, COLOR_PAIR(C_BLUE) | A_BOLD);  mvwprintw(win, 1, 2,  "JMP");  wattroff(win, COLOR_PAIR(C_BLUE) | A_BOLD);
    wattron(win, COLOR_PAIR(C_GREEN) | A_BOLD); mvwprintw(win, 1, 6,  "BPB");  wattroff(win, COLOR_PAIR(C_GREEN) | A_BOLD);
    wattron(win, COLOR_PAIR(C_NORMAL));          mvwprintw(win, 1, 10, "CODE"); wattroff(win, COLOR_PAIR(C_NORMAL));
    wattron(win, COLOR_PAIR(C_RED) | A_BOLD);   mvwprintw(win, 1, 15, "SIG");  wattroff(win, COLOR_PAIR(C_RED) | A_BOLD);
    wattron(win, COLOR_PAIR(C_GRAY));            mvwprintw(win, 1, max_w-10, "%03X/%03X", scroll*16, 512); wattroff(win, COLOR_PAIR(C_GRAY));

    int lines = max_h - 3;
    for (int row = 0; row < lines; row++) {
        int base = (row + scroll) * 16;
        if (base >= 512) break;
        wattron(win, COLOR_PAIR(C_GRAY));
        mvwprintw(win, row+2, 2, "%03X", base);
        mvwprintw(win, row+2, 6, "|");
        wattroff(win, COLOR_PAIR(C_GRAY));
        for (int col = 0; col < 16; col++) {
            int idx = base + col; if (idx >= 512) break;
            int color = (idx<3)?C_BLUE:(idx<90)?C_GREEN:(idx>=510)?C_RED:C_NORMAL;
            wattron(win, COLOR_PAIR(color)|(color==C_NORMAL?0:A_BOLD));
            mvwprintw(win, row+2, 7+col*3, "%02X", sector[idx]);
            wattroff(win, COLOR_PAIR(color)|(color==C_NORMAL?0:A_BOLD));
        }
        wattron(win, COLOR_PAIR(C_GRAY));
        mvwprintw(win, row+2, 7+16*3, "|");
        wattroff(win, COLOR_PAIR(C_GRAY));
        for (int col = 0; col < 16; col++) {
            int idx = base+col; if (idx>=512) break;
            char c = isprint(sector[idx])?(char)sector[idx]:'.';
            wattron(win, COLOR_PAIR(C_GRAY));
            mvwprintw(win, row+2, 56+col, "%c", c);
            wattroff(win, COLOR_PAIR(C_GRAY));
        }
    }
}

void draw_fat(WINDOW *win, BPB *bpb, const char *filename, int scroll) {
    int max_h, max_w; getmaxyx(win, max_h, max_w); (void)max_w;
    uint32_t fat_offset = (uint32_t)bpb->rsvd_sec_cnt * bpb->bytes_per_sec;
    uint32_t fat_size   = bpb->fat_sz32 * bpb->bytes_per_sec;
    uint32_t max_clus   = fat_size / 4;
    uint32_t *fat = malloc(fat_size);
    if (!fat) { mvwprintw(win, 2, 2, "malloc failed"); return; }
    FILE *f = fopen(filename, "rb");
    if (!f) { free(fat); mvwprintw(win, 2, 2, "Erreur ouverture"); return; }
    fseek(f, fat_offset, SEEK_SET);
    size_t rd = fread(fat, 1, fat_size, f); fclose(f);
    if (rd < fat_size) { mvwprintw(win, 2, 2, "Lecture FAT incomplete"); free(fat); return; }

    wattron(win, COLOR_PAIR(C_GRAY));    mvwprintw(win, 1, 2,  "FREE"); wattroff(win, COLOR_PAIR(C_GRAY));
    wattron(win, COLOR_PAIR(C_GREEN));   mvwprintw(win, 1, 8,  "USED"); wattroff(win, COLOR_PAIR(C_GREEN));
    wattron(win, COLOR_PAIR(C_RED));     mvwprintw(win, 1, 14, "END");  wattroff(win, COLOR_PAIR(C_RED));
    wattron(win, COLOR_PAIR(C_MAGENTA)); mvwprintw(win, 1, 19, "BAD");  wattroff(win, COLOR_PAIR(C_MAGENTA));
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, 2, 2,  "%-6s", "Clus");
    mvwprintw(win, 2, 9,  "%-12s", "Valeur FAT");
    mvwprintw(win, 2, 22, "%-8s", "Etat");
    wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);

    typedef struct { uint32_t clus; uint32_t val; int is_summary; uint32_t free_count; } FatRow;
    FatRow *rows = malloc(max_clus * sizeof(FatRow));
    if (!rows) { free(fat); return; }
    int total_rows = 0;
    for (uint32_t i = 2; i < max_clus; i++) {
        uint32_t val = fat[i] & 0x0FFFFFFF;
        if (val == 0) {
            uint32_t run = 1;
            while (i+run < max_clus && (fat[i+run] & 0x0FFFFFFF) == 0) run++;
            if (run > 3) { rows[total_rows++] = (FatRow){i,0,1,run}; i += run-1; }
            else           rows[total_rows++] = (FatRow){i,val,0,0};
        } else rows[total_rows++] = (FatRow){i,val,0,0};
    }

    wattron(win, COLOR_PAIR(C_GRAY));
    mvwprintw(win, 1, max_w-14, "%d/%d", scroll, total_rows);
    wattroff(win, COLOR_PAIR(C_GRAY));

    int lines = max_h - 4;
    for (int r = 0; r < lines && r+scroll < total_rows; r++) {
        FatRow *row = &rows[r+scroll];
        int srow = r+3;
        if (row->is_summary) {
            wattron(win, COLOR_PAIR(C_GRAY));
            mvwprintw(win, srow, 2, "%u clusters libres", row->free_count);
            wattroff(win, COLOR_PAIR(C_GRAY));
            continue;
        }
        uint32_t val = row->val;
        const char *state; int color;
        if      (val == 0)          { state="FREE"; color=C_GRAY;    }
        else if (val >= 0x0FFFFFF8) { state="END";  color=C_RED;     }
        else if (val == 0x0FFFFFF7) { state="BAD";  color=C_MAGENTA; }
        else                        { state="USED"; color=C_GREEN;   }
        wattron(win, COLOR_PAIR(C_GRAY));  mvwprintw(win, srow, 2, "%-6u", row->clus); wattroff(win, COLOR_PAIR(C_GRAY));
        wattron(win, COLOR_PAIR(color)|A_BOLD);
        mvwprintw(win, srow, 9,  val==0?"0x00000000  ":"0x%08X  ", fat[row->clus]);
        mvwprintw(win, srow, 22, "%-8s", state);
        wattroff(win, COLOR_PAIR(color)|A_BOLD);
    }
    free(rows); free(fat);
}

void draw_kernel(WINDOW *win, BPB *bpb, const char *filename) {
    int max_h, max_w; getmaxyx(win, max_h, max_w); (void)max_w;
    uint32_t bps=bpb->bytes_per_sec, spc=bpb->sec_per_clus;
    uint32_t fat_size=bpb->fat_sz32*bps;
    FILE *f = fopen(filename, "rb");
    if (!f) { mvwprintw(win, 2, 2, "Erreur ouverture"); return; }
    uint32_t *fat = load_fat(f, bpb);
    uint32_t first_cluster=0, file_size=0;
    find_file_in_root(f, bpb, "KERNEL  BIN", &first_cluster, &file_size);
    fclose(f);

    if (first_cluster == 0) {
        wattron(win, COLOR_PAIR(C_RED)|A_BOLD); mvwprintw(win, 2, 2, "KERNEL  BIN introuvable"); wattroff(win, COLOR_PAIR(C_RED)|A_BOLD);
        if (fat) free(fat); return;
    }
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);  mvwprintw(win, 1, 2,  "KERNEL  BIN");         wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    wattron(win, COLOR_PAIR(C_GRAY));          mvwprintw(win, 1, 15, "taille:");              wattroff(win, COLOR_PAIR(C_GRAY));
    wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD); mvwprintw(win, 1, 23, "%u octets", file_size); wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, 2, 2,  "%-4s",  "#");
    mvwprintw(win, 2, 7,  "%-8s",  "Cluster");
    mvwprintw(win, 2, 16, "%-8s",  "LBA");
    mvwprintw(win, 2, 25, "%-14s", "Offset");
    mvwprintw(win, 2, 40, "CHS");
    mvwprintw(win, 2, 52, "Suivant");
    wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);

    uint32_t clus=first_cluster, max_clus=fat_size/4;
    int row=3, idx=0;
    while (fat && clus>=2 && clus<0x0FFFFFF7 && row<max_h-1) {
        uint32_t lba    = (uint32_t)bpb->rsvd_sec_cnt + bpb->num_fats*bpb->fat_sz32 + (clus-2)*spc;
        uint32_t offset = lba*bps;
        uint32_t next   = (clus<max_clus)?(fat[clus]&0x0FFFFFFF):0;
        uint32_t cyl=lba/(255*63), head=(lba/63)%255, sect=(lba%63)+1;

        wattron(win, COLOR_PAIR(C_GRAY));           mvwprintw(win, row, 2,  "%-4d",       idx);    wattroff(win, COLOR_PAIR(C_GRAY));
        wattron(win, COLOR_PAIR(C_GREEN)|A_BOLD);   mvwprintw(win, row, 7,  "%-8u",       clus);   wattroff(win, COLOR_PAIR(C_GREEN)|A_BOLD);
        wattron(win, COLOR_PAIR(C_YELLOW));          mvwprintw(win, row, 16, "%-8u",       lba);    wattroff(win, COLOR_PAIR(C_YELLOW));
        wattron(win, COLOR_PAIR(C_GRAY));            mvwprintw(win, row, 25, "0x%08X    ", offset); wattroff(win, COLOR_PAIR(C_GRAY));
        wattron(win, COLOR_PAIR(C_CYAN));            mvwprintw(win, row, 40, "C%u H%u S%u", cyl, head, sect); wattroff(win, COLOR_PAIR(C_CYAN));
        if (next>=0x0FFFFFF8) {
            wattron(win, COLOR_PAIR(C_RED)|A_BOLD); mvwprintw(win, row, 52, "END"); wattroff(win, COLOR_PAIR(C_RED)|A_BOLD);
        } else {
            wattron(win, COLOR_PAIR(C_GREEN)); mvwprintw(win, row, 52, "-> %u", next); wattroff(win, COLOR_PAIR(C_GREEN));
        }
        clus=next; row++; idx++;
    }
    if (fat) free(fat);
}

static void read_cmdbar(int rows, char *out, int out_size) {
    move(rows-1, 0); clrtoeol();
    attron(COLOR_PAIR(C_CYAN)|A_BOLD); printw(": "); attroff(COLOR_PAIR(C_CYAN)|A_BOLD);
    echo(); curs_set(1);
    getnstr(out, out_size-1);
    noecho(); curs_set(0);
    move(rows-1, 0); clrtoeol();
}

static void show_error(int rows, const char *msg) {
    attron(COLOR_PAIR(C_RED)|A_BOLD);
    mvprintw(rows-1, 0, "%s", msg);
    attroff(COLOR_PAIR(C_RED)|A_BOLD);
    refresh(); napms(1200);
    move(rows-1, 0); clrtoeol();
}

static void overlay_check(int rows, int cols, const char *filename) {
    uint8_t sector[512];
    FILE *f = fopen(filename, "rb");
    if (!f) return;
    fread(sector, 1, 512, f);
    BPB *bpb = (BPB *)sector;

    int h=18, w=62, y=(rows-h)/2, x=(cols-w)/2;
    WINDOW *win = newwin(h, w, y, x);
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD); box(win,0,0); wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    wattron(win, COLOR_PAIR(C_SELECTED)|A_BOLD); mvwprintw(win, 0, (w-9)/2, " --check "); wattroff(win, COLOR_PAIR(C_SELECTED)|A_BOLD);

    int errors=0, row=2;
    #define WCHECK(cond, label, ok, fail) \
        wattron(win, COLOR_PAIR((cond)?C_GREEN:C_RED)|A_BOLD); \
        mvwprintw(win, row, 2, (cond)?"[OK]":"[!!]"); \
        wattroff(win, COLOR_PAIR((cond)?C_GREEN:C_RED)|A_BOLD); \
        wattron(win, COLOR_PAIR(C_CYAN)); \
        mvwprintw(win, row, 7, "%-26s", label); \
        wattroff(win, COLOR_PAIR(C_CYAN)); \
        wattron(win, COLOR_PAIR((cond)?C_GRAY:C_YELLOW)|A_BOLD); \
        mvwprintw(win, row, 34, (cond)?ok:fail); \
        wattroff(win, COLOR_PAIR((cond)?C_GRAY:C_YELLOW)|A_BOLD); \
        if (!(cond)) errors++; row++;

    WCHECK(sector[510]==0x55&&sector[511]==0xAA, "Signature 55AA",         "OK",           "MANQUANTE")
    WCHECK(bpb->bytes_per_sec==512,              "Bytes per sector",       "512",          "!=512")
    WCHECK(bpb->sec_per_clus>0&&(bpb->sec_per_clus&(bpb->sec_per_clus-1))==0, "Sec per cluster", "puissance de 2", "invalide")
    WCHECK(bpb->num_fats==2,                     "Num FATs",               "2",            "!=2")
    WCHECK(bpb->root_ent_cnt==0,                 "Root entry count",       "0 (FAT32 OK)", "!=0")
    WCHECK(bpb->fat_sz16==0,                     "FAT16 size",             "0 (FAT32 OK)", "!=0")
    WCHECK(bpb->fat_sz32>0,                      "FAT32 size",             "OK",           "=0 corrompu")
    WCHECK(bpb->root_clus>=2,                    "Root cluster",           "OK",           "<2 invalide")
    WCHECK(bpb->rsvd_sec_cnt>=32,                "Reserved sectors",       "OK",           "<32")

    uint8_t sec1[512];
    fseek(f, 512, SEEK_SET);
    fread(sec1, 1, 512, f);
    int fsinfo = (sec1[0]=='R'&&sec1[1]=='R'&&sec1[2]=='a'&&sec1[3]=='A');
    WCHECK(!fsinfo, "Secteur 1 libre",            "stage2 OK",    "FSInfo ecrase stage2!")

    uint32_t kclus=0, ksize=0;
    int found = find_file_in_root(f, bpb, "KERNEL  BIN", &kclus, &ksize);
    WCHECK(found, "KERNEL  BIN present",          "OK",           "introuvable")
    fclose(f);

    row++;
    if (errors==0) {
        wattron(win, COLOR_PAIR(C_GREEN)|A_BOLD); mvwprintw(win, row, 2, "OK — image valide, pret a booter"); wattroff(win, COLOR_PAIR(C_GREEN)|A_BOLD);
    } else {
        wattron(win, COLOR_PAIR(C_RED)|A_BOLD); mvwprintw(win, row, 2, "%d erreur(s) detectee(s)", errors); wattroff(win, COLOR_PAIR(C_RED)|A_BOLD);
    }
    wattron(win, COLOR_PAIR(C_GRAY)); mvwprintw(win, h-2, (w-18)/2, "appuie sur une touche"); wattroff(win, COLOR_PAIR(C_GRAY));
    wrefresh(win); getch(); delwin(win);
}

static void overlay_file(int rows, int cols, const char *filename, const char *name) {
    FILE *f = fopen(filename, "rb");
    if (!f) return;
    BPB bpb; fread(&bpb, sizeof(BPB), 1, f);
    char name83[12]; to_83(name, name83);
    uint32_t first_cluster=0, file_size=0;
    if (!find_file_in_root(f, &bpb, name83, &first_cluster, &file_size)) { fclose(f); return; }
    uint32_t *fat = load_fat(f, &bpb);
    fclose(f);
    if (!fat) return;

    uint32_t bps=bpb.bytes_per_sec, spc=bpb.sec_per_clus;
    uint32_t fat_size=bpb.fat_sz32*bps, max_clus=fat_size/4;

    int h=rows-4, w=cols-4;
    WINDOW *win = newwin(h, w, 2, 2);
    wattron(win, COLOR_PAIR(C_MAGENTA)|A_BOLD); box(win,0,0); wattroff(win, COLOR_PAIR(C_MAGENTA)|A_BOLD);
    char title[64]; snprintf(title, sizeof(title), " --file %s ", name);
    wattron(win, COLOR_PAIR(C_SELECTED)|A_BOLD); mvwprintw(win, 0, (w-(int)strlen(title))/2, "%s", title); wattroff(win, COLOR_PAIR(C_SELECTED)|A_BOLD);

    wattron(win, COLOR_PAIR(C_GRAY));  mvwprintw(win, 1, 2, "taille:"); wattroff(win, COLOR_PAIR(C_GRAY));
    wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD); mvwprintw(win, 1, 10, "%u octets  premier cluster: %u", file_size, first_cluster); wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, 2, 2, "%-4s", "#"); mvwprintw(win, 2, 7, "%-8s", "Cluster");
    mvwprintw(win, 2, 16, "%-8s", "LBA"); mvwprintw(win, 2, 25, "%-14s", "Offset disque");
    mvwprintw(win, 2, 40, "CHS"); mvwprintw(win, 2, 55, "Suivant");
    wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);

    uint32_t clus=first_cluster; int row=3, idx=0;
    while (clus>=2 && clus<0x0FFFFFF7 && row<h-2) {
        uint32_t lba    = (uint32_t)bpb.rsvd_sec_cnt + bpb.num_fats*bpb.fat_sz32 + (clus-2)*spc;
        uint32_t offset = lba*bps;
        uint32_t next   = (clus<max_clus)?(fat[clus]&0x0FFFFFFF):0;
        uint32_t cyl=lba/(255*63), head=(lba/63)%255, sect=(lba%63)+1;

        wattron(win, COLOR_PAIR(C_GRAY));           mvwprintw(win, row, 2,  "%-4d",       idx);    wattroff(win, COLOR_PAIR(C_GRAY));
        wattron(win, COLOR_PAIR(C_GREEN)|A_BOLD);   mvwprintw(win, row, 7,  "%-8u",       clus);   wattroff(win, COLOR_PAIR(C_GREEN)|A_BOLD);
        wattron(win, COLOR_PAIR(C_YELLOW));          mvwprintw(win, row, 16, "%-8u",       lba);    wattroff(win, COLOR_PAIR(C_YELLOW));
        wattron(win, COLOR_PAIR(C_GRAY));            mvwprintw(win, row, 25, "0x%08X    ", offset); wattroff(win, COLOR_PAIR(C_GRAY));
        wattron(win, COLOR_PAIR(C_CYAN));            mvwprintw(win, row, 40, "C%u H%u S%u", cyl, head, sect); wattroff(win, COLOR_PAIR(C_CYAN));
        if (next>=0x0FFFFFF8) {
            wattron(win, COLOR_PAIR(C_RED)|A_BOLD); mvwprintw(win, row, 55, "END"); wattroff(win, COLOR_PAIR(C_RED)|A_BOLD);
        } else {
            wattron(win, COLOR_PAIR(C_GREEN)); mvwprintw(win, row, 55, "-> %u", next); wattroff(win, COLOR_PAIR(C_GREEN));
        }
        clus=next; row++; idx++;
    }
    wattron(win, COLOR_PAIR(C_GRAY)); mvwprintw(win, h-2, (w-18)/2, "appuie sur une touche"); wattroff(win, COLOR_PAIR(C_GRAY));
    wrefresh(win); getch(); delwin(win); free(fat);
}

static void overlay_map(int rows, int cols, const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) return;
    BPB bpb; fread(&bpb, sizeof(BPB), 1, f);

    uint32_t bps=bpb.bytes_per_sec, spc=bpb.sec_per_clus;
    uint32_t fat_start=bpb.rsvd_sec_cnt, fat_size=bpb.fat_sz32;
    uint32_t data_sec=fat_start+bpb.num_fats*fat_size, total=bpb.tot_sec32;

    uint8_t sec1[512];
    fseek(f, 512, SEEK_SET);
    fread(sec1, 1, 512, f);
    int fsinfo=(sec1[0]=='R'&&sec1[1]=='R'&&sec1[2]=='a'&&sec1[3]=='A');

    uint32_t *fat = load_fat(f, &bpb);
    uint32_t max_clus = fat ? (bpb.fat_sz32*bps)/4 : 0;

    uint32_t root_off = ((uint32_t)bpb.rsvd_sec_cnt + bpb.num_fats*bpb.fat_sz32)*bps + (bpb.root_clus-2)*spc*bps;
    uint8_t dir[512];
    fseek(f, root_off, SEEK_SET);
    fread(dir, 1, 512, f);
    fclose(f);

    int h=rows-4, w=cols-4;
    WINDOW *win = newwin(h, w, 2, 2);
    wattron(win, COLOR_PAIR(C_YELLOW)|A_BOLD); box(win,0,0); wattroff(win, COLOR_PAIR(C_YELLOW)|A_BOLD);
    wattron(win, COLOR_PAIR(C_SELECTED)|A_BOLD); mvwprintw(win, 0, (w-7)/2, " --map "); wattroff(win, COLOR_PAIR(C_SELECTED)|A_BOLD);
    wattron(win, COLOR_PAIR(C_CYAN)|A_BOLD);
    mvwprintw(win, 1, 2, "%-8s  %-8s  %-8s  %-10s  Zone", "Debut", "Fin", "Taille", "Octets");
    wattroff(win, COLOR_PAIR(C_CYAN)|A_BOLD);

    int row=2;
    #define MROW(d,e,sz,ob,color,label) \
        wattron(win,COLOR_PAIR(C_GRAY)); mvwprintw(win,row,2,"%-8u  %-8u  %-8u  %-10u  ",d,e,sz,ob); wattroff(win,COLOR_PAIR(C_GRAY)); \
        wattron(win,COLOR_PAIR(color)|A_BOLD); mvwprintw(win,row,44,label); wattroff(win,COLOR_PAIR(color)|A_BOLD); row++;

    MROW(0,0,1,bps, C_BLUE, "MBR / Stage1")
    if (fsinfo) {
        wattron(win,COLOR_PAIR(C_GRAY)); mvwprintw(win,row,2,"%-8u  %-8u  %-8u  %-10u  ",1,1,1,bps); wattroff(win,COLOR_PAIR(C_GRAY));
        wattron(win,COLOR_PAIR(C_RED)|A_BOLD); mvwprintw(win,row,44,"FSInfo [stage2 ecrase!]"); wattroff(win,COLOR_PAIR(C_RED)|A_BOLD); row++;
    } else {
        MROW(1,1,1,bps, C_GREEN, "Stage2")
    }
    if (fat_start>2) { MROW(2,fat_start-1,fat_start-2,(fat_start-2)*bps, C_CYAN, "Secteurs reserves") }
    MROW(fat_start, fat_start+fat_size-1, fat_size, fat_size*bps, C_YELLOW, "FAT1")
    uint32_t fat2=fat_start+fat_size;
    MROW(fat2, fat2+fat_size-1, fat_size, fat_size*bps, C_YELLOW, "FAT2 (backup)")
    uint32_t root_lba=data_sec+(bpb.root_clus-2)*spc;
    wattron(win,COLOR_PAIR(C_GRAY)); mvwprintw(win,row,2,"%-8u  %-8u  %-8u  %-10u  ",root_lba,root_lba+spc-1,spc,spc*bps); wattroff(win,COLOR_PAIR(C_GRAY));
    wattron(win,COLOR_PAIR(C_MAGENTA)|A_BOLD); mvwprintw(win,row,44,"Repertoire racine (cluster %u)",bpb.root_clus); wattroff(win,COLOR_PAIR(C_MAGENTA)|A_BOLD); row++;

    if (fat) {
        for (int e=0; e<512 && row<h-3; e+=32) {
            if (dir[e]==0x00) break;
            if (dir[e]==0xE5||dir[e+11]==0x0F) continue;
            char name[13]={0};
            memcpy(name,&dir[e],8);
            for(int i=7;i>=0&&name[i]==' ';i--) name[i]='\0';
            if(dir[e+8]!=' '){int nl=strlen(name);name[nl]='.';memcpy(name+nl+1,&dir[e+8],3);name[nl+4]='\0';for(int i=nl+3;i>nl+1&&name[i]==' ';i--)name[i]='\0';}
            uint16_t hi=*(uint16_t*)&dir[e+0x14], lo=*(uint16_t*)&dir[e+0x1A];
            uint32_t clus=((uint32_t)hi<<16)|lo, size=*(uint32_t*)&dir[e+0x1C];
            int nclus=0; uint32_t c=clus;
            while(c>=2&&c<0x0FFFFFF7&&c<max_clus){nclus++;c=fat[c]&0x0FFFFFFF;}
            uint32_t flba=data_sec+(clus-2)*spc;
            char label[64]; snprintf(label,sizeof(label),"%-12s  %u octets  %d cluster(s)",name,size,nclus);
            wattron(win,COLOR_PAIR(C_GRAY)); mvwprintw(win,row,2,"%-8u  %-8u  %-8u  %-10u  ",flba,flba+(uint32_t)nclus*spc-1,(uint32_t)nclus*spc,(uint32_t)nclus*spc*bps); wattroff(win,COLOR_PAIR(C_GRAY));
            wattron(win,COLOR_PAIR(C_GREEN)|A_BOLD); mvwprintw(win,row,44,"%s",label); wattroff(win,COLOR_PAIR(C_GREEN)|A_BOLD); row++;
        }
        uint32_t free_clus=0;
        for(uint32_t i=2;i<max_clus;i++) if((fat[i]&0x0FFFFFFF)==0) free_clus++;
        wattron(win,COLOR_PAIR(C_GRAY)); mvwprintw(win,row,2,"%-8s  %-8u  %-8u  %-10u  ","...",total-1,free_clus*spc,free_clus*spc*bps); wattroff(win,COLOR_PAIR(C_GRAY));
        wattron(win,COLOR_PAIR(C_GRAY)); mvwprintw(win,row,44,"Espace libre (%u clusters)",free_clus); wattroff(win,COLOR_PAIR(C_GRAY)); row++;
        wattron(win,COLOR_PAIR(C_GRAY)); mvwprintw(win,row,2,"Total: %u secteurs -- %u Mo",total,(total*bps)/(1024*1024)); wattroff(win,COLOR_PAIR(C_GRAY));
        free(fat);
    }
    wattron(win,COLOR_PAIR(C_GRAY)); mvwprintw(win,h-2,(w-18)/2,"appuie sur une touche"); wattroff(win,COLOR_PAIR(C_GRAY));
    wrefresh(win); getch(); delwin(win);
}

void compute_layout(int rows, int cols, int *top_h, int *bot_h, int *left_w, int *right_w,
                    int py[], int px[], int ph[], int pw[]) {
    *top_h=(rows-3)*6/10; *bot_h=(rows-3)-*top_h;
    *left_w=cols/3;        *right_w=cols-*left_w;
    py[0]=py[1]=3; py[2]=py[3]=3+*top_h;
    px[0]=px[2]=0; px[1]=px[3]=*left_w;
    ph[0]=ph[1]=*top_h; ph[2]=ph[3]=*bot_h;
    pw[0]=pw[2]=*left_w; pw[1]=pw[3]=*right_w;
}

int main(int argc, char *argv[]) {
    char filename[512];
    strncpy(filename, argc>1?argv[1]:"boot.img", sizeof(filename)-1);
    FILE *f=fopen(filename,"rb");
    if(!f){fprintf(stderr,"Erreur: impossible d'ouvrir %s\n",filename);return 1;}
    BPB bpb; fread(&bpb,sizeof(BPB),1,f); fclose(f);

    initscr(); noecho(); curs_set(0); keypad(stdscr,TRUE); init_colors();
    refresh();
    int rows,cols; getmaxyx(stdscr,rows,cols);
    int top_h,bot_h,left_w,right_w;
    int py[4],px[4],ph[4],pw[4];
    compute_layout(rows,cols,&top_h,&bot_h,&left_w,&right_w,py,px,ph,pw);

    int active=0, hex_scroll=0, fat_scroll=0;
    const char *titles[]={" BPB "," Hex View "," FAT Clusters "," kernel.bin "};

    while(1){
        clear();
        draw_header(filename,cols);
        WINDOW *wins[4];
        for(int i=0;i<4;i++){
            wins[i]=make_panel(ph[i],pw[i],py[i],px[i],titles[i],i==active,panel_colors[i]);
            refresh(); wrefresh(wins[i]);
        }
        draw_bpb(wins[0],&bpb);                      wrefresh(wins[0]);
        draw_hex(wins[1],&bpb,filename,hex_scroll);   wrefresh(wins[1]);
        draw_fat(wins[2],&bpb,filename,fat_scroll);   wrefresh(wins[2]);
        draw_kernel(wins[3],&bpb,filename);            wrefresh(wins[3]);

        int ch=getch();
        for(int i=0;i<4;i++) delwin(wins[i]);

        if(ch=='q'||ch=='Q') break;
        if(ch==KEY_RESIZE){getmaxyx(stdscr,rows,cols);compute_layout(rows,cols,&top_h,&bot_h,&left_w,&right_w,py,px,ph,pw);}
        if(ch==KEY_RIGHT) active=(active+1)%4;
        if(ch==KEY_LEFT)  active=(active-1+4)%4;
        if(ch=='j'||ch==KEY_DOWN){
            if(active==1){int mx=32-(top_h-3);if(hex_scroll<mx)hex_scroll++;}
            if(active==2) fat_scroll++;
        }
        if(ch=='k'||ch==KEY_UP){
            if(active==1&&hex_scroll>0) hex_scroll--;
            if(active==2&&fat_scroll>0) fat_scroll--;
        }

        if(ch==':'){
            char input[512]={0};
            read_cmdbar(rows, input, sizeof(input));

            if(strncmp(input,"open ",5)==0){
                char *path=input+5;
                FILE *nf=fopen(path,"rb");
                if(nf){
                    strncpy(filename,path,sizeof(filename)-1);
                    fread(&bpb,sizeof(BPB),1,nf); fclose(nf);
                    hex_scroll=0; fat_scroll=0;
                } else {
                    char msg[600]; snprintf(msg,sizeof(msg),"erreur: impossible d'ouvrir %s",path);
                    show_error(rows,msg);
                }
            } else if(strcmp(input,"--check")==0){
                overlay_check(rows,cols,filename);
            } else if(strncmp(input,"--file ",7)==0){
                overlay_file(rows,cols,filename,input+7);
            } else if(strcmp(input,"--map")==0){
                overlay_map(rows,cols,filename);
            } else if(input[0]!='\0'){
                char msg[600]; snprintf(msg,sizeof(msg),"commande inconnue: %s  (open / --check / --file <nom> / --map)",input);
                show_error(rows,msg);
            }
        }
    }
    endwin();
    return 0;
}