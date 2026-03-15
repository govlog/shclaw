# shclaw

*[Read in English](README.md)*

## C'est quoi ce truc

J'en avais marre du bloatware. Chaque "framework d'agents IA" que j'ai regarde tirait la moitie de npm ou avait besoin d'un virtualenv Python de la taille d'un petit pays. Je voulais un truc qui *tourne*, point. Pas de runtime, pas d'interpreteur, pas de gestionnaire de paquets, pas de conteneur obligatoire. Juste un binaire et un noyau.

Donc j'ai ecrit shclaw : un orchestrateur multi-agents IA en C qui compile en un seul binaire statique de moins de 1Mo. Il parle aux LLMs (Claude, GPT, Ollama), vit sur IRC, planifie ses propres taches, se souvient de trucs entre les sessions, et — le truc que je trouve vraiment cool — il embarque un compilateur C pour que les agents puissent ecrire et compiler leurs propres plugins a la volee. Le tout tourne nativement sur Linux, FreeBSD, NetBSD et OpenBSD a partir du meme binaire ELF.

C'est un projet perso. Un jouet. Un truc sur lequel je bidouille parce que le probleme est interessant et parce qu'il y a quelque chose de satisfaisant a faire tenir TLS, HTTP, IRC, du parsing JSON, une boucle agentique et un compilateur C dans 955 kilo-octets.

C'est aussi en grande partie du vibe-coding. J'avais les briques, l'architecture dans la tete, et assez de connaissances en C et en systeme pour garder le cap — mais le gros du code a ete ecrit a coups de Claude, Codex, et moi qui gueule sur les deux jusqu'a ce que le binaire linke.

> **Attention.** Shclaw donne a des agents IA autonomes l'acces a des commandes shell, a l'ecriture de fichiers et a des appels reseau. L'un d'entre eux peut ecrire et compiler du C a la volee. Faites-le tourner sur un truc dont vous vous fichez — une VM, un conteneur, un Raspberry Pi sur un VLAN.

```
~6000 lignes de C · ~518K statique (musl) · ~956K multi-plateforme (cosmo) · 4 systemes d'exploitation
```

---

## Comment ca marche

```
    agents          hub · recherche · builder (1 pthread chacun)
      │                     │
      ▼                     ▼
  event loop ◄──── poll() sur IRC fd + unix socket fd
      │               timer 5s : planificateur, inbox, scan plugins
      │
      ├── IRC (TLS 6697)        ← le proprio parle aux agents
      ├── unix socket           ← TUI / CLI
      ├── planificateur + inbox ← taches planifiees, messages inter-agents
      └── scanner plugins       ← recompile les .c modifies via TCC
```

Chaque agent tourne dans son propre pthread. Un declencheur (mention IRC, tache planifiee, message inter-agent, commande CLI) cree un thread de session. L'agent construit un prompt systeme a partir de sa personnalite, sa memoire, son planning et la liste de ses pairs, puis entre dans une boucle de conversation avec le fournisseur LLM. Quand le LLM renvoie des appels d'outils, il les execute et reinjecte les resultats. Quand il renvoie du texte, la reponse part sur IRC ou le socket TUI. Les sessions ont une limite de tours configurable (defaut 100) et un cooldown de 5 secondes entre les sessions d'un meme agent.

La boucle d'evenements est un seul appel `poll()` qui surveille le socket IRC et le socket Unix, avec un timer de 5 secondes pour la livraison des messages, la planification des taches et le scan du repertoire plugins.

### Roles des agents

Chaque agent est defini par un fichier INI dans `etc/agents/`. Deux flags controlent des comportements speciaux :

- **`hub = true`** — Cet agent est le destinataire par defaut. Tout message IRC qui ne commence pas par un `@mention` est route vers le hub. Il ne doit y en avoir qu'un — c'est "l'accueil" qui gere la conversation generale et decide quand deleguer aux specialistes via `send_message`. Le hub n'a pas besoin d'un gros modele — un truc comme `qwen3.5:9b` via Ollama ou `gpt-4.1-nano` marche bien pour le routage et la conversation courante.
- **`builder = true`** — Cet agent a acces a l'outil `create_plugin`. Il peut ecrire du code source C et le faire compiler par le daemon en outil live via TCC. On ne veut pas que tous les agents aient ca — c'est puissant et dangereux, donc on le donne a un agent dedie avec un prompt systeme qui connait les contraintes de l'API plugin (`-nostdlib`, `tc_plugin.h` uniquement, pas de libc). **Il faut un modele costaud pour ca** (Claude Sonnet/Opus, GPT-4.1) — les petits modeles vont halluciner des headers libc ou produire du code qui ne compile pas. Utilisez le template `builder.ini.example`, il contient assez d'instructions dans `system_prompt_extra` pour garder le modele sur les rails.

Un setup typique : un agent hub (generaliste, modele pas cher/local), un agent recherche (analyse, modele standard), et un agent builder (creation de plugins, modele costaud). Ils se coordonnent via `send_message` — des boites aux lettres fichier dans `data/messages/<agent>/`, consultees toutes les 5 secondes par le daemon.

Pour l'instant, les seuls canaux de communication sont IRC et le socket Unix (TUI/CLI). Je prevois d'ajouter d'autres canaux prochainement.

### Memoire a deux etages

Chaque agent a deux mecanismes de persistance independants :

**Memoire** (`data/<agent>/memory/memory.jsonl`) — Un log JSONL en append-only. Quand un agent appelle `remember`, un objet JSON est ajoute avec le contenu, une categorie (`general`, `project`, `person`, `event`, `error`), un score d'importance (1-10), des tags, et un timestamp. Quand l'agent appelle `recall`, il cherche par correspondance de mots-cles et de tags. Les N derniers souvenirs sont injectes dans le prompt systeme au debut de chaque session, pour que l'agent ait du contexte sur les interactions passees.

**Faits** (`data/<agent>/memory/facts.json`) — Un objet JSON plat cle-valeur. Quand un agent appelle `set_fact`, il stocke une association permanente (`timezone` = `Europe/Paris`, `owner_name` = `Alice`). Les faits sont toujours inclus dans le prompt systeme — c'est le "savoir dur" de l'agent qui n'est jamais elague. Contrairement aux souvenirs qui s'accumulent et qui devront eventuellement etre tailles, les faits sont censes etre peu nombreux, precis et permanents.

L'idee : les souvenirs c'est pour le rappel episodique ("mardi dernier le serveur a eu un souci de disque"), les faits c'est pour l'identite et la configuration ("le proprio parle francais", "le serveur de prod c'est 10.0.1.5").

### Le tour de passe-passe TCC

La piece la plus inhabituelle de shclaw, c'est qu'il embarque [TinyCC](https://bellard.org/tcc/) (libtcc) — le compilateur C de Fabrice Bellard — comme bibliotheque. Quand un agent veut creer un nouvel outil, il ecrit du code source C. Le daemon le passe a `tcc_compile_string()`, le reloge en memoire avec `tcc_relocate()`, et resout le point d'entree avec `tcc_get_symbol()`. Aucun fichier `.so` n'est jamais ecrit sur le disque. Le code compile vit dans l'espace d'adressage du processus et est appelable immediatement.

Les plugins tournent en mode `-nostdlib` : pas de libc, pas de headers systeme. Le daemon injecte un ensemble de fonctions selectionnees (`tc_malloc`, `tc_http_get`, `tc_json_parse`, etc.) via `tcc_add_symbol()` avant la compilation. C'est a la fois un sandbox (les plugins ne peuvent pas appeler de fonctions libc arbitraires) et une commodite (les plugins ont du HTTP avec TLS gratuitement sans rien linker).

Au redemarrage, `plugin_scan()` recompile tous les fichiers `.c` dans `plugins/`. Les fichiers modifies sont detectes par mtime et recompiles a la volee toutes les 5 secondes.

### Comment un ELF tourne sur quatre noyaux

Le build musl produit un binaire statique Linux standard (~518K, durci avec static-PIE, RELRO, NX, stack protector, FORTIFY_SOURCE). Ca marche sur x86_64, aarch64 et armv7l — la detection d'architecture est automatique. J'ai teste sur un Raspberry Pi 3B+ en Raspbian 32 bits et ca tourne nickel (static-pie est desactive sur armv7l a cause de soucis musl/noyau, ca retombe sur du `-static` classique).

Le build Cosmopolitan est plus interessant. [Cosmopolitan Libc](https://justine.lol/cosmopolitan/) de Justine Tunney fournit une libc qui cible plusieurs systemes d'exploitation a partir du meme binaire. On compile avec `x86_64-unknown-cosmo-cc` (pas le `cosmocc` fat — TinyCC genere des relocations ELF x86_64, donc on a besoin de la toolchain specifique x86_64). L'etape `assimilate` convertit le format APE (Actually Portable Executable) en ELF natif que le noyau charge directement — pas de trampoline shell, pas d'interpreteur.

Le resultat : un ELF de 956K qui tourne tel quel sur Linux, NetBSD, FreeBSD et OpenBSD. Meme binaire, quatre noyaux. Par flemme je n'ai teste que Linux et NetBSD pour l'instant — FreeBSD et OpenBSD c'est sur la liste, j'y viendrai.

Faire fonctionner TCC sous Cosmopolitan a necessite trois patches (`patches/tcc-cosmo.patch`) :

1. **Guard NULL dans `tcc_split_path()`** — Cosmopolitan ne definit pas `tcc_lib_path` par defaut ; le dereferencer fait crasher. Fix : fallback sur `"."`.
2. **`strcpy` remplace par `memcpy` pour les noms PLT** — le `strcpy` de Cosmopolitan utilise des instructions SSE (`memrchr16_sse`) qui plantent sur certains petits buffers alignes sur la pile. Celui-la etait fun a traquer.
3. **Ignorer les noms de section vides** — `tcc_add_linker_symbols()` genere des symboles en double pour les sections dont le nom est vide apres suppression du point.

### Ce qu'il y a dedans

| Composant | Projet | Role |
|-----------|--------|------|
| TLS 1.2 | [BearSSL](https://bearssl.org/) par Thomas Pornin | HTTPS vers les APIs LLM, IRC sur TLS. Pas d'allocation dynamique, pas de dependances. |
| Compilateur C | [TinyCC](https://bellard.org/tcc/) par Fabrice Bellard | Compilation de plugins en memoire. libtcc embarque comme bibliotheque. |
| JSON | [cJSON](https://github.com/DaveGamble/cJSON) par Dave Gamble | Parse/genere toutes les payloads des APIs LLM. Un seul fichier, ~1700 lignes. |
| Libc | [musl](https://musl.libc.org/) ou [Cosmopolitan](https://justine.lol/cosmopolitan/) | Linkage statique. musl pour Linux, Cosmopolitan pour le multi-plateforme. |

Tout le reste (client HTTP/1.1, client IRC, parseur INI, TUI ANSI, planificateur, systeme de memoire, scanner de plugins) est ecrit from scratch — environ 6000 lignes de C reparties dans 20 fichiers source.

---

## Pour commencer

### Dependances

Debian/Ubuntu :
```bash
sudo apt install build-essential musl-tools git
```

C'est tout. Le Makefile recupere automatiquement les sources vendorisees ([BearSSL](https://bearssl.org/), [TinyCC](https://bellard.org/tcc/), [cJSON](https://github.com/DaveGamble/cJSON)) au premier build via `vendor.sh`.

Pour le build Cosmopolitan, la toolchain est aussi recuperee automatiquement (~40Mo, cache dans `vendor/cosmo/`).

### Compiler

```bash
git clone https://github.com/YOUR/shclaw.git && cd shclaw

# Linux uniquement (musl-gcc, statique, durci)
make musl          # => ./shclaw (~518K)

# Multi-plateforme (Cosmopolitan, x86_64)
make cosmo         # => ./shclaw.com (~956K, tourne sur Linux/NetBSD/FreeBSD/OpenBSD)

make clean         # tout nettoyer
```

### Configurer

Shclaw utilise des fichiers INI plats. Pas de YAML, pas de JSON config, pas de variables d'environnement.

```bash
mkdir -p etc/agents
```

**`etc/config.ini`** — configuration globale (fournisseurs, tiers, IRC, chemins) :

```ini
[daemon]
data_dir = ./data
log_dir  = ./logs

[provider.anthropic]
type    = anthropic
api_key = sk-ant-api03-VOTRE-CLE-ICI

[provider.ollama]
type     = openai
base_url = http://localhost:11434
api_key  =

[tiers]
simple   = anthropic/claude-haiku-4-5-20251001
standard = anthropic/claude-sonnet-4-6
complex  = anthropic/claude-opus-4-6
local    = ollama/llama3

# Optionnel — supprimez cette section pour tourner sans IRC
[irc]
server      = irc.libera.chat
port        = 6697
nick        = monbot
channel     = #mon-channel
channel_key = maclesecrete
owner       = monnick
```

**`etc/agents/jarvis.ini`** — un fichier par agent :

```ini
[agent]
name       = jarvis
model      = simple
hub        = true
max_turns  = 30
personality = Tu es Jarvis, un assistant efficace et direct.

[objectives]
1 = Repondre aux demandes rapidement
2 = Garder en memoire les taches importantes
```

Voir `etc/config.ini.example` et `etc/agents/*.ini.example` pour la reference complete.

### Lancer

#### Bare metal

```bash
./shclaw                  # premier plan
./shclaw -d               # demoniser (fork + setsid)
./shclaw tui              # interface chat terminal (se connecte au daemon)
./shclaw tui oracle       # parler a un agent specifique
./shclaw msg jarvis "yo"  # message fire-and-forget
./shclaw status           # etat des agents (JSON)
./shclaw stop             # arret propre
```

Le daemon cherche `etc/config.ini` dans le repertoire courant. Changer avec `--workdir=/chemin/vers/instance`.

#### Docker

```bash
# Construire l'image
make docker-image     # => shclaw:latest (~11Mo Alpine)

# Preparer un repertoire d'instance
mkdir -p my-instance/{etc/agents,data,logs,plugins}
# copier et editer config.ini + fichiers .ini des agents dans my-instance/etc/

# Lancer
docker run -v ./my-instance:/app/instance shclaw
```

#### smolBSD (microVM NetBSD)

[smolBSD](https://github.com/NetBSDfr/smolBSD) par NetBSDfr fait booter une VM NetBSD minimale en ~60ms via QEMU. Le binaire Cosmopolitan de shclaw tourne nativement dessus.

```bash
# Recuperer smolBSD (premiere fois)
./vendor.sh smolbsd

# Construire l'image microVM (necessite le binaire cosmo)
make smolbsd AGENT_DIR=/chemin/vers/instance

# Booter
cd vendor/smolbsd
./startnb.sh -k kernels/netbsd-SMOL -i images/shclaw-amd64.img -w /chemin/vers/instance
```

Le repertoire d'instance est monte via 9P virtfs sur `/mnt`. La VM boote, monte le repertoire hote, et lance shclaw en environ une seconde.

#### Installation systeme

```bash
sudo make install PREFIX=/opt/shclaw

# Le prefixe est integre au binaire — il cherche toujours la-bas pour la config
/opt/shclaw/bin/shclaw
```

Cree `$PREFIX/{bin,etc,etc/agents,plugins,include,data,logs}`.

---

## Outils des agents

Les agents disposent de 15 outils integres :

| Outil | Ce qu'il fait |
|-------|---------------|
| `exec` | Executer une commande shell (fork+exec, capture pipe, timeout, kill pgid) |
| `read_file` | Lire le contenu d'un fichier (avec offset et limite) |
| `write_file` | Ecrire/ajouter a un fichier (atomique : tmp + rename) |
| `schedule_task` | Tache ponctuelle a une heure ISO 8601 |
| `schedule_recurring` | Tache recurrente (30min / horaire / 6h / 12h / quotidien / hebdo) |
| `list_tasks` | Lister les taches planifiees |
| `update_task` | Modifier une tache existante |
| `cancel_task` | Annuler une tache par ID |
| `remember` | Sauvegarder un souvenir (tague, categorise, score d'importance) |
| `recall` | Chercher dans les souvenirs par mot-cle |
| `set_fact` | Stocker un fait cle-valeur permanent |
| `get_fact` | Recuperer un fait |
| `send_message` | Envoyer un message a un autre agent, au proprietaire (via IRC), ou en broadcast |
| `list_agents` | Lister les agents actifs |
| `create_plugin` | Ecrire du C + compiler via TCC (agents builder uniquement) |

Les plugins ecrits par les agents deviennent des outils disponibles pour tous les agents immediatement.

---

## API Plugin

Les plugins sont des fichiers `.c` unitaires. Ils font `#include "tc_plugin.h"` et exportent trois symboles :

```c
#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "weather";
const char *TC_PLUGIN_DESC = "Recuperer la meteo pour une ville";

const char *tc_execute(const char *input_json) {
    void *json = tc_json_parse(input_json);
    const char *city = tc_json_string(tc_json_get(json, "city"));

    char url[256];
    tc_snprintf(url, sizeof(url), "https://wttr.in/%s?format=3", city);

    static char result[512];
    int status = tc_http_get(url, result, sizeof(result));
    tc_json_free(json);

    return (status == 200) ? result : "erreur";
}
```

Fonctions disponibles (injectees par le daemon a la compilation) :

| Categorie | Fonctions |
|-----------|-----------|
| Memoire | `tc_malloc`, `tc_free` |
| Chaines | `tc_strlen`, `tc_strcmp`, `tc_strncmp`, `tc_strcpy`, `tc_strncpy`, `tc_snprintf`, `tc_memcpy`, `tc_memset` |
| Fichiers | `tc_read_file`, `tc_write_file` |
| HTTP | `tc_http_get`, `tc_http_post`, `tc_http_post_json`, `tc_http_header` |
| JSON | `tc_json_parse`, `tc_json_free`, `tc_json_print`, `tc_json_get`, `tc_json_index`, `tc_json_array_size`, `tc_json_string` |
| Systeme | `tc_gethostname` |
| Logging | `tc_log` |

Les appels HTTP utilisent la stack BearSSL du daemon — les plugins ont HTTPS gratuitement.

---

## IRC

Connexion unique, un seul pseudo, un seul canal. Les agents sont multiplexes sur la meme connexion.

```
vous>   hey, verifie la charge du serveur        => route vers l'agent hub
vous>   @oracle analyse ce fichier de log         => route vers oracle
vous>   @oracle X @jarvis Y                       => fan-out vers les deux
vous>   @all status                               => broadcast

bot>    jarvis: CPU a 12%, tout va bien.
bot>    oracle: Je vois 3 anomalies dans le log...
```

TLS est automatique sur le port 6697 (BearSSL). Si aucun canal/cle n'est configure, shclaw en genere des aleatoires — les recuperer avec `shclaw irc-info`.

---

## Etat de la documentation

C'est le projet perso d'une seule personne. Le code fait office de documentation pour l'instant. Si le projet vous interesse, que vous voulez le faire tourner ou contribuer, n'hesitez pas a ouvrir une issue — je suis ouvert a ecrire de meilleure doc pour les parties qui interessent les gens.

Les coups de main sont les bienvenus :
- Packaging pour d'autres distros (Alpine, Arch, NixOS)
- Tests ARM64 sur NetBSD/FreeBSD
- Plus d'exemples de plugins
- Documentation en general

---

## Credits

Ce projet n'existerait pas sans le travail de :

- **[Justine Tunney](https://justine.lol/)** — [Cosmopolitan Libc](https://justine.lol/cosmopolitan/) et le format APE. L'idee qu'un seul binaire puisse tourner sur quatre systemes d'exploitation, pour de vrai, sans emulation.
- **[Fabrice Bellard](https://bellard.org/)** — [TinyCC](https://bellard.org/tcc/). Un compilateur C complet assez petit pour etre embarque comme bibliotheque. C'est ce qui rend la compilation de plugins a la volee possible.
- **[Thomas Pornin](https://www.bearssl.org/)** — [BearSSL](https://bearssl.org/). Une implementation TLS concue pour l'embarque : petite, sans allocation, crypto en temps constant. C'est grace a ca qu'un binaire de moins de 1Mo peut parler HTTPS.
- **[Dave Gamble](https://github.com/DaveGamble/cJSON)** — [cJSON](https://github.com/DaveGamble/cJSON). Parseur JSON en un seul fichier. Simple, fiable, gere tout ce que les APIs LLM lui balancent.
- **[iMil](https://x.com/iMilnb)** — [smolBSD](https://github.com/NetBSDfr/smolBSD). Framework de microVM NetBSD minimale. Booter un BSD complet en 60ms.

## Licence

MIT. Voir [LICENSE](LICENSE).

Les bibliotheques vendorisees ont leurs propres licences :

| Bibliotheque | Licence |
|--------------|---------|
| BearSSL | MIT |
| cJSON | MIT |
| TinyCC | LGPL-2.1 |
| musl libc | MIT |
| Cosmopolitan Libc | ISC |

Voir [NOTICE](NOTICE) pour les details.
