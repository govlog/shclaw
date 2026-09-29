# Fonctionnement interne

Détails techniques pour les contributeurs et les curieux.

## La boucle d'événements

Le daemon tourne sur une seule boucle `poll()` qui surveille le socket IRC, le socket Unix (CLI et TUI) et les clients TUI attachés, avec un tick d'une seconde. IRC est optionnel ; quand le lien tombe, la boucle continue de servir le socket et se reconnecte en arrière-plan : 5 s après une coupure, puis un délai qui double jusqu'à 5 min tant que le serveur n'accepte pas l'enregistrement.

Chaque déclencheur lance une session dans son propre thread. Un agent traite une session à la fois, avec 5 secondes entre deux sessions :

- les messages du propriétaire (IRC, TUI, CLI) attendent dans une file que leur agent soit libre ;
- le courrier inter-agents et les tâches échues restent sur le disque jusqu'à ce que l'agent soit libre : rien n'est perdu ni tronqué pendant qu'il travaille ;
- les sources des plugins sont rescannées toutes les 5 secondes.

Dans une session, l'agent construit un prompt système à partir de sa personnalité, de ses faits, de ses souvenirs récents, de son planning et de la liste de ses pairs, puis boucle avec le LLM jusqu'à ce qu'il réponde sans appeler d'outil (limite par défaut : 100 tours).

## Comment le harnais aide les petits modèles

Les petits modèles bouclent, inventent des outils et envoient des arguments cassés. La boucle de session détecte ces cas et répond par une erreur exploitable :

- des arguments qui ne sont pas un objet JSON, ou qui ne respectent pas le schéma de l'outil (paramètre manquant, mauvais type, valeur hors enum), reçoivent la ligne d'usage de l'outil ; les nombres et booléens envoyés en chaîne sont acceptés ;
- un outil inconnu reçoit la liste des outils disponibles ;
- le troisième appel identique (même outil, mêmes arguments) est refusé, et une session qui se répète est arrêtée ;
- une réponse vide reçoit une relance ;
- une réponse texte à la demande d'un autre agent lui est renvoyée automatiquement, marquée comme réponse, donc sans boucle de réponses ;
- une tâche échue est présentée comme « à faire maintenant », et une tâche ponctuelle dans le passé est refusée (les modèles reprogrammaient un rappel échu au lieu de le délivrer) ;
- `schedule_task` accepte `in_minutes`, que les petits modèles remplissent bien plus souvent correctement qu'une date absolue.

Avec Anthropic, les tours de l'assistant sont rejoués tels que reçus (les blocs de réflexion gardent leur signature) et l'historique ne fait que s'allonger. La requête active le cache de prompt automatique : chaque tour d'outils ne paie plein tarif que ce qu'il ajoute. Avec les fournisseurs compatibles OpenAI, `history_budget` raccourcit les anciennes sorties d'outils quand une session dépasse une petite fenêtre de contexte. Les deux fournisseurs réessaient deux fois les erreurs de connexion, 408, 409, 429 et 5xx, en respectant `Retry-After`.

## Compilation des plugins

shclaw embarque [TinyCC](https://bellard.org/tcc/) (libtcc) comme bibliothèque. Quand un agent appelle `create_plugin`, le daemon :

1. Compile le source une première fois dans un processus fils : un plantage du compilateur sur une entrée tordue, ou un symbole invalide, ne fait pas tomber le daemon
2. Le compile en mémoire avec `tcc_compile_string()` et le reloge avec `tcc_relocate()`
3. Résout `TC_PLUGIN_NAME`, `tc_execute`, la description et le schéma optionnel avec `tcc_get_symbol()`
4. N'écrit le source `.c` dans `plugins/` qu'à ce moment : une tentative ratée ne remplace jamais un plugin qui marche
5. Le lance une fois avec `test_input`, si fourni, et renvoie la sortie et les appels HTTP

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
