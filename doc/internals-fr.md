# Fonctionnement interne

Détails techniques pour les contributeurs et les curieux.

## La boucle d'événements

Le daemon tourne sur un seul appel `poll()` qui surveille le socket IRC et le socket Unix (TUI/CLI). Un timer de 5 secondes gère la livraison des messages, la planification des tâches et le scan du répertoire plugins.

Chaque agent tourne dans son propre pthread. Un déclencheur (mention IRC, tâche planifiée, message inter-agent, commande CLI) crée un thread de session. L'agent construit un prompt système à partir de sa personnalité, sa mémoire, son planning et la liste de ses pairs, puis entre dans une boucle de conversation avec le LLM. Les sessions ont une limite de tours configurable (défaut 100) et un cooldown de 5 secondes.

## Compilation des plugins

shclaw embarque [TinyCC](https://bellard.org/tcc/) (libtcc) comme bibliothèque. Quand un agent appelle `create_plugin`, le daemon :

1. Écrit le source `.c` dans `plugins/`
2. Le passe à `tcc_compile_string()`
3. Le reloge en mémoire avec `tcc_relocate()`
4. Résout le point d'entrée avec `tcc_get_symbol()`

Aucun `.so` n'est écrit sur le disque. Au redémarrage, `plugin_scan()` recompile tous les `.c`. Les fichiers modifiés sont détectés par mtime et recompilés toutes les 5 secondes.

## Binaire multi-plateforme (Cosmopolitan)

Le build musl produit un binaire Linux statique (~528K, durci avec static-PIE, RELRO, NX, stack protector, FORTIFY_SOURCE). Marche sur x86_64, aarch64 et armv7l.

Le build [Cosmopolitan](https://justine.lol/cosmopolitan/) produit un ELF de ~968K qui tourne sur Linux, NetBSD, FreeBSD et OpenBSD. On compile avec `x86_64-unknown-cosmo-cc` (TCC génère des relocations ELF x86_64). L'étape `assimilate` convertit le format APE en ELF natif.

### Patches TCC pour Cosmopolitan

Trois patches appliqués automatiquement (`patches/tcc-cosmo.patch`) :

1. **Guard NULL dans `tcc_split_path()`** -- Cosmopolitan ne définit pas `tcc_lib_path` par défaut.
2. **`strcpy` remplacé par `memcpy` pour les noms PLT** -- le `strcpy` de Cosmopolitan utilise des instructions SSE qui plantent sur certains petits buffers.
3. **Ignorer les noms de section vides** -- `tcc_add_linker_symbols()` génère des symboles en double.

## Ce qu'il y a dedans

| Composant | Projet | Rôle |
|-----------|--------|------|
| TLS 1.2 | [BearSSL](https://bearssl.org/) par Thomas Pornin | HTTPS vers les APIs LLM, IRC sur TLS |
| Compilateur C | [TinyCC](https://bellard.org/tcc/) par Fabrice Bellard | Compilation de plugins en mémoire |
| JSON | [cJSON](https://github.com/DaveGamble/cJSON) par Dave Gamble | Parsing des payloads LLM |
| Libc | [musl](https://musl.libc.org/) ou [Cosmopolitan](https://justine.lol/cosmopolitan/) | Linkage statique |

Le reste (client HTTP, client IRC, parseur INI, TUI, planificateur, mémoire, scanner de plugins) est écrit from scratch -- environ 6000 lignes de C.
