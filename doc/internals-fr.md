# Fonctionnement interne

Détails techniques pour les contributeurs et les curieux.

## La boucle d'événements

Le daemon tourne sur une seule boucle `poll()` qui surveille le socket IRC, le socket Unix (CLI et TUI) et les clients TUI attachés, avec un tick d'une seconde. IRC est optionnel ; quand le lien tombe, la boucle continue de servir le socket et se reconnecte en arrière-plan : 5 s après une coupure, puis un délai qui double jusqu'à 5 min tant que le serveur n'accepte pas l'enregistrement.

Chaque déclencheur lance une session dans son propre thread. Un agent traite une session à la fois, avec 5 secondes entre deux sessions :

- les messages du propriétaire (IRC, TUI, CLI) attendent dans une file que leur agent soit libre ;
- le courrier inter-agents et les tâches échues restent sur le disque jusqu'à ce que l'agent soit libre : rien n'est perdu ni tronqué pendant qu'il travaille ;
- les sources des plugins sont rescannées toutes les 5 secondes.

Dans une session, l'agent envoie un prompt système qui ne change qu'avec sa configuration (personnalité, règles, objectifs, liste des pairs, modèle de plugin pour le builder) et un premier tour utilisateur avec le contexte de la session (règles de communication, faits, planning, souvenirs récents, heure) suivi du déclencheur. Il boucle ensuite avec le LLM jusqu'à ce qu'il réponde sans appeler d'outil (limite par défaut : 100 tours).

## Comment le harnais aide les petits modèles

Les petits modèles bouclent, inventent des outils et envoient des arguments cassés. La boucle de session détecte ces cas et répond par une erreur exploitable :

- des arguments qui ne sont pas un objet JSON, ou qui ne respectent pas le schéma de l'outil (paramètre manquant, mauvais type, valeur hors enum), reçoivent la ligne d'usage de l'outil ; les nombres et booléens envoyés en chaîne sont acceptés ;
- un outil inconnu reçoit la liste des outils disponibles ;
- le troisième appel identique (même outil, mêmes arguments) est refusé, et une session qui se répète est arrêtée ;
- une réponse vide reçoit une relance ;
- une réponse texte à la demande d'un autre agent lui est renvoyée automatiquement, marquée comme réponse, donc sans boucle de réponses ;
- une tâche échue est présentée comme « à faire maintenant », et une tâche ponctuelle dans le passé est refusée (les modèles reprogrammaient un rappel échu au lieu de le délivrer) ;
- `schedule_task` accepte `in_minutes`, que les petits modèles remplissent bien plus souvent correctement qu'une date absolue.

Avec Anthropic, les tours de l'assistant sont rejoués tels que reçus (les blocs de réflexion gardent leur signature) et l'historique ne fait que s'allonger. Avec les fournisseurs compatibles OpenAI, `history_budget` raccourcit les anciennes sorties d'outils quand une session dépasse une petite fenêtre de contexte. Les deux fournisseurs réessaient deux fois les erreurs de connexion, 408, 409, 429 et 5xx, en respectant `Retry-After`.

## Comment les prompts restent en cache

Un fournisseur réutilise le travail déjà fait sur un début de prompt qu'il a déjà vu : Anthropic et OpenAI facturent ces tokens une fraction du prix (souvent un dixième), et un serveur local (Ollama, llama.cpp) ne les recalcule pas, ce qui compte surtout sur CPU. Chaque requête garde ce préfixe aussi long que possible :

- les outils viennent en premier : outils intégrés dans un ordre fixe, puis plugins triés par nom. Le prompt système suit et ne contient rien qui change d'une session à l'autre ;
- le premier tour utilisateur va du plus stable au plus variable : type de déclencheur et règles de communication, faits, planning, souvenirs récents, heure, puis le message. Avant GPT-5.6, OpenAI ne réutilise son cache que si peu de tokens suivent la première différence avec une requête précédente (sur gpt-4.1-nano : succès avec 50 tokens après, échec avec 100), d'où l'heure et le message en dernier ;
- l'historique ne fait que s'allonger, sauf avec `history_budget` : il raccourcit les anciennes sorties d'outils jusqu'à la moitié du budget en une fois, donc le préfixe change rarement ;
- Anthropic : un point de cache ferme le prompt système, donc les sessions suivantes réutilisent outils et prompt système ; le cache automatique couvre la conversation. OpenAI : `prompt_cache_key` vaut `shclaw-<agent>`. Les agents ont les mêmes outils en tête de prompt ; sans la clé, ils partagent un seul groupe de routage (environ 15 requêtes par minute) et débordent vers des serveurs qui n'ont pas leur cache.

Chaque appel au modèle journalise sa consommation, par exemple `[oracle] Tokens: 1430 in (1280 from cache, 0 to cache), 8 out`. Un préfixe sous le minimum du fournisseur n'est jamais mis en cache : 1024 tokens chez OpenAI, 512 à 4096 chez Claude selon le modèle.

## Compilation des plugins

shclaw embarque [TinyCC](https://bellard.org/tcc/) (libtcc) comme bibliothèque. Quand un agent appelle `create_plugin`, le daemon :

1. Décode le source une fois de plus s'il arrive sur une seule ligne avec des `\n` littéraux (les petits modèles échappent parfois le code deux fois)
2. Compile le source une première fois dans un processus fils : un plantage du compilateur sur une entrée tordue, ou un symbole invalide, ne fait pas tomber le daemon
3. Le compile en mémoire avec `tcc_compile_string()` et le reloge avec `tcc_relocate()`
4. Résout `TC_PLUGIN_NAME`, `tc_execute`, la description et le schéma optionnel avec `tcc_get_symbol()`
5. N'écrit le source `.c` dans `plugins/` qu'à ce moment : une tentative ratée ne remplace jamais un plugin qui marche
6. Le lance une fois avec `test_input`, si fourni, et renvoie la sortie et les appels HTTP, avec une note quand une réponse ne tenait pas dans le buffer du plugin

Aucun `.so` n'est écrit sur le disque. Au redémarrage, `plugin_scan()` recompile tous les `.c`. Toutes les 5 secondes, les fichiers modifiés (détectés par mtime) sont recompilés et les fichiers supprimés sont déchargés.

Chaque appel de plugin tourne dans un processus fils avec un délai maximal de 180 secondes.

## Binaire multi-plateforme (Cosmopolitan)

Le build musl produit un binaire Linux statique (~530K, durci avec static-PIE, RELRO, NX, stack protector). Marche sur x86_64, aarch64 et armv7l.

Le build [Cosmopolitan](https://justine.lol/cosmopolitan/) produit un ELF de ~970K qui tourne sur Linux, NetBSD, FreeBSD et OpenBSD. On compile avec `x86_64-unknown-cosmo-cc` (TCC génère des relocations ELF x86_64). L'étape `assimilate` convertit le format APE en ELF natif.

Deux pièges de Cosmopolitan façonnent le code :

- Certaines fonctions de chaînes (`strstr`, `strcpy`...) sont des IFUNC : prendre leur adresse les casse dans tout le programme. Les plugins reçoivent donc de petites fonctions d'enveloppe, jamais les adresses de la libc.
- `sscanf` ne gère pas les scansets comme `%*1[T ]` : le parseur de dates est écrit à la main.

`make check-cosmo` lance les vérifications avec le build Cosmopolitan.

### Patches TCC pour Cosmopolitan

Trois patches appliqués automatiquement (`patches/tcc-cosmo.patch`) :

1. **Guard NULL dans `tcc_split_path()`** -- Cosmopolitan ne définit pas `tcc_lib_path` par défaut.
2. **`strcpy` remplacé par `memcpy` pour les noms PLT** -- le `strcpy` de Cosmopolitan utilise des instructions SSE qui plantent sur certains petits buffers.
3. **Ignorer les noms de section vides** -- `tcc_add_linker_symbols()` génère des symboles en double.

## Sources vendorisées

`vendor.sh` récupère des révisions épinglées : un commit pour BearSSL, TinyCC et smolBSD, un commit plus un SHA-256 pour cJSON, une version plus un SHA-256 pour la chaîne cosmocc. La branche `mob` de TinyCC accepte les pushs de n'importe qui : rien n'est donc compilé depuis une tête non épinglée. Pour mettre à jour, changez l'épingle, supprimez `vendor/<nom>` et recompilez.

## Ce qu'il y a dedans

| Composant | Projet | Rôle |
|-----------|--------|------|
| TLS 1.2 | [BearSSL](https://bearssl.org/) par Thomas Pornin | HTTPS vers les APIs LLM, IRC sur TLS |
| Compilateur C | [TinyCC](https://bellard.org/tcc/) par Fabrice Bellard | Compilation de plugins en mémoire |
| JSON | [cJSON](https://github.com/DaveGamble/cJSON) par Dave Gamble | Parsing des payloads LLM |
| Libc | [musl](https://musl.libc.org/) ou [Cosmopolitan](https://justine.lol/cosmopolitan/) | Linkage statique |

Le reste (client HTTP, client IRC, parseur INI, TUI, planificateur, mémoire, scanner de plugins) est écrit from scratch -- environ 7000 lignes de C.
