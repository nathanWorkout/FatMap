# fatmap

Outil TUI pour inspecter les images disque FAT32 directement dans le terminal.

## Aperçu

fatmap affiche en temps réel la structure interne d'une image FAT32 :
BPB, dump hexadécimal, table d'allocation FAT, chaîne de clusters du kernel,
carte complète du disque — le tout dans une interface TUI colorée et navigable.

## Interface

fatmap est divisé en 4 cadres :

### BPB — BIOS Parameter Block
Affiche les champs essentiels du BPB avec leur offset hexadécimal :
- Bytes Per Sector
- Sectors Per Cluster
- Reserved Sectors
- Num FATs
- Total Sectors
- FAT Size
- Root Cluster

### Hex View — Dump hexadécimal
Affiche le secteur 0 octet par octet, coloré par zone :
- Bleu : instruction JMP (octets 0-2)
- Vert : BPB (octets 3-89)
- Blanc : code du bootloader
- Rouge : signature 55AA (octets 510-511)

### FAT Clusters — Table d'allocation
Liste tous les clusters de la FAT32 avec leur état :
- `FREE` — cluster libre
- `USED` — cluster utilisé, pointe vers le suivant
- `END`  — dernier cluster d'une chaîne
- `BAD`  — cluster défectueux

Les longues plages de clusters libres sont compressées en une seule ligne.

### kernel.bin — Chaîne de clusters
Affiche la chaîne complète des clusters du fichier `KERNEL.BIN` :
cluster, LBA, offset disque, adresse CHS, pointeur vers le suivant.

## Navigation

| Touche | Action |
|--------|--------|
| `←` / `→` | Changer de cadre actif |
| `j` / `↓` | Scroller vers le bas |
| `k` / `↑` | Scroller vers le haut |
| `q` | Quitter |

## Commandes

| Commande | Description |
|----------|-------------|
| `:open <chemin>` | Ouvrir une nouvelle image disque |
| `:--check` | Valider l'image (BPB, signature, kernel...) |
| `:--file <nom>` | Afficher la chaîne de clusters d'un fichier |
| `:--map` | Carte complète du disque (MBR, FAT, fichiers, espace libre) |

### Détail de `:--check`
Vérifie que l'image est valide et bootable :
- Signature 55AA présente
- Bytes per sector = 512
- Sectors per cluster = puissance de 2
- Num FATs = 2
- Root entry count = 0 (FAT32)
- FAT32 size non nulle
- Root cluster >= 2
- Reserved sectors >= 32
- Secteur 1 libre (non écrasé par FSInfo)
- KERNEL.BIN présent

### Détail de `:--map`
Affiche la disposition complète du disque secteur par secteur :
MBR/Stage1, Stage2, secteurs réservés, FAT1, FAT2, répertoire racine,
chaque fichier avec sa taille et son nombre de clusters, espace libre, total.

## Licence

MIT
