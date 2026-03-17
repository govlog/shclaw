<div align="center">

# shclaw

**Orchestrateur multi-agents IA autonome en C.**
**Un seul binaire statique. Zéro dépendance. Zéro runtime.**

<br>

![C](https://img.shields.io/badge/C11-00599C?style=flat-square&logo=c&logoColor=white)
![License](https://img.shields.io/badge/Licence-MIT-green?style=flat-square)
![musl](https://img.shields.io/badge/musl-528K-blue?style=flat-square)
![cosmo](https://img.shields.io/badge/cosmo-968K-blue?style=flat-square)
![Lines](https://img.shields.io/badge/~6000_lignes-grey?style=flat-square)
![Linux](https://img.shields.io/badge/Linux-FCC624?style=flat-square&logo=linux&logoColor=black)
![FreeBSD](https://img.shields.io/badge/FreeBSD-AB2B28?style=flat-square&logo=freebsd&logoColor=white)
![NetBSD](https://img.shields.io/badge/NetBSD-FF6600?style=flat-square&logo=netbsd&logoColor=white)
![OpenBSD](https://img.shields.io/badge/OpenBSD-F2CA30?style=flat-square&logo=openbsd&logoColor=black)

<br>

*IRC · Multi-LLM · Plugins C à la volée · Planification · Messagerie inter-agents*

<br>

<img src="screen.png" alt="shclaw TUI" width="700">

</div>

<br>

*[Read in English](README.md)*

## C'est quoi ce truc

Un orchestrateur multi-agents IA en C. Un seul binaire statique de moins de 1Mo. Il parle aux LLMs (Claude, GPT, Ollama), vit sur IRC, planifie ses propres tâches, se souvient de trucs entre les sessions, et embarque un compilateur C pour que les agents puissent écrire et compiler leurs propres plugins à la volée.

Tourne sur Linux, FreeBSD, NetBSD et OpenBSD à partir du même binaire.

> **Attention.** Shclaw donne à des agents IA l'accès à des commandes shell, à l'écriture de fichiers et à des appels réseau. L'un d'entre eux peut écrire et compiler du C à la volée. Faites-le tourner sur un truc dont vous vous fichez -- une VM, un conteneur, un Pi sur un VLAN.

---

## Comment ça marche

Le daemon tourne une boucle d'événements qui surveille IRC et un socket Unix. Les déclencheurs (messages IRC, tâches planifiées, messages inter-agents, commandes CLI) créent des sessions. Chaque agent a son propre thread, une personnalité, une mémoire persistante, et accès à des outils.

Les agents se coordonnent via `send_message` -- des boîtes aux lettres fichier consultées toutes les 5 secondes.

### Types d'agents

Chaque agent est défini par un fichier INI dans `etc/agents/`. Deux flags comptent :

- **`hub = true`** -- Destinataire par défaut des messages IRC sans `@mention`. Gère la conversation générale, délègue aux spécialistes. Marche bien avec des petits modèles (`qwen3.5:9b`, `gpt-4.1-nano`).
- **`builder = true`** -- Peut créer des plugins C à la volée via `create_plugin`. Ne voit que 4 outils pour rester concentré. Le template plugin est pré-injecté dans son prompt, et le daemon extrait automatiquement le code si le modèle le sort en texte. Même des petits modèles comme `qwen3.5:9b` arrivent à créer des plugins fonctionnels du premier coup.

Un setup typique : un hub (modèle pas cher/local), un agent recherche (modèle standard), un builder (modèle costaud).

### Mémoire

Chaque agent a deux couches de persistance :

- **Souvenirs** -- Log en append-only avec catégories, scores d'importance et tags. Recherchés via `recall`. Les souvenirs récents sont inclus dans le prompt système.
- **Faits** -- Paires clé-valeur permanentes (ex : `timezone = Europe/Paris`). Toujours dans le prompt système. Peu nombreux, précis, jamais supprimés.

### Plugins à la volée

Les agents peuvent écrire des plugins C qui sont compilés en mémoire par [TinyCC](https://bellard.org/tcc/). Aucun `.so` n'est écrit sur le disque. Les plugins tournent en sandbox (pas de libc) mais ont accès à HTTP+TLS, JSON et I/O fichier via des fonctions `tc_*` injectées.

Voir [doc/plugin-api-fr.md](doc/plugin-api-fr.md) pour l'API plugin complète.

---

## Pour commencer

### Compiler

```bash
sudo apt install build-essential musl-tools git  # Debian/Ubuntu
git clone https://github.com/govlog/shclaw.git && cd shclaw

make musl          # Linux uniquement, ~528K
make cosmo         # Multi-plateforme (Linux/NetBSD/FreeBSD/OpenBSD), ~968K
```

Les bibliothèques vendorisées sont récupérées automatiquement au premier build.

### Configurer

```bash
mkdir -p etc/agents
```

**`etc/config.ini`** -- fournisseurs, IRC, chemins :

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

# Optionnel -- supprimez cette section pour tourner sans IRC
[irc]
server      = irc.libera.chat
port        = 6697
nick        = monbot
channel     = #mon-channel
channel_key = maclesecrete
owner       = monnick
```

**`etc/agents/jarvis.ini`** -- un fichier par agent :

```ini
[agent]
name       = jarvis
model      = simple
hub        = true
max_turns  = 30
personality = Tu es Jarvis, un assistant efficace et direct.

[objectives]
1 = Répondre aux demandes rapidement
2 = Garder en mémoire les tâches importantes
```

Voir `etc/agents/*.ini.example` pour plus d'exemples.

### Lancer

```bash
./shclaw                  # premier plan
./shclaw -d               # démoniser
./shclaw tui              # interface chat terminal
./shclaw tui oracle       # parler à un agent spécifique
./shclaw msg jarvis "yo"  # envoyer un message
./shclaw status           # état des agents (JSON)
./shclaw stop             # arrêt propre
```

Le daemon cherche `etc/config.ini` dans le répertoire courant. Changer avec `--workdir=/chemin/vers/instance`.

### Docker

```bash
make docker-image
docker run -v ./my-instance:/app/instance shclaw
```

### smolBSD (microVM NetBSD)

Le binaire Cosmopolitan tourne sur [smolBSD](https://github.com/NetBSDfr/smolBSD) -- une VM NetBSD minimale qui boote en ~60ms.

```bash
make smolbsd AGENT_DIR=/chemin/vers/instance
cd vendor/smolbsd && ./startnb.sh -k kernels/netbsd-SMOL \
  -i images/shclaw-amd64.img -w /chemin/vers/instance
```

---

## Outils des agents

16 outils intégrés :

| Outil | Ce qu'il fait |
|-------|---------------|
| `exec` | Exécuter une commande shell |
| `read_file` | Lire un fichier |
| `write_file` | Écrire/ajouter à un fichier |
| `schedule_task` | Tâche ponctuelle à une heure donnée |
| `schedule_recurring` | Tâche récurrente |
| `list_tasks` | Lister les tâches planifiées |
| `update_task` | Modifier une tâche |
| `cancel_task` | Annuler une tâche |
| `remember` | Sauvegarder un souvenir |
| `recall` | Chercher dans les souvenirs |
| `set_fact` | Stocker un fait clé-valeur |
| `get_fact` | Récupérer un fait |
| `send_message` | Envoyer un message à un agent, au proprio, ou en broadcast |
| `list_agents` | Lister les agents actifs |
| `create_plugin` | Écrire + compiler un plugin C (builder uniquement) |
| `clear_memory` | Effacer souvenirs/faits |

Les plugins créés par les agents deviennent des outils disponibles pour tous immédiatement.

---

## IRC

Connexion unique, un pseudo, un canal. Les agents partagent la connexion.

```
vous>   hey, vérifie la charge du serveur        => routé vers le hub
vous>   @oracle analyse ce fichier de log         => routé vers oracle
vous>   @oracle X @jarvis Y                       => chacun reçoit sa partie
vous>   @all status                               => broadcast

bot>    jarvis: CPU à 12%, tout va bien.
bot>    oracle: Je vois 3 anomalies dans le log...
```

---

## Documentation

- [API Plugin](doc/plugin-api-fr.md) -- écrire des plugins, fonctions `tc_*` disponibles
- [Fonctionnement interne](doc/internals-fr.md) -- boucle d'événements, compilation TCC, build Cosmopolitan

---

## Crédits

- **[Justine Tunney](https://justine.lol/)** -- [Cosmopolitan Libc](https://justine.lol/cosmopolitan/). Un binaire, quatre systèmes d'exploitation.
- **[Fabrice Bellard](https://bellard.org/)** -- [TinyCC](https://bellard.org/tcc/). Un compilateur C assez petit pour être embarqué comme bibliothèque.
- **[Thomas Pornin](https://www.bearssl.org/)** -- [BearSSL](https://bearssl.org/). TLS pour l'embarqué.
- **[Dave Gamble](https://github.com/DaveGamble/cJSON)** -- [cJSON](https://github.com/DaveGamble/cJSON). Parseur JSON en un fichier.
- **[iMil](https://x.com/iMilnb)** -- [smolBSD](https://github.com/NetBSDfr/smolBSD). MicroVMs NetBSD.

## Licence

MIT. Voir [LICENSE](LICENSE).

Bibliothèques vendorisées : BearSSL (MIT), cJSON (MIT), TinyCC (LGPL-2.1), musl (MIT), Cosmopolitan (ISC). Voir [NOTICE](NOTICE).
