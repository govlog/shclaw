# API Plugin

Les plugins sont des fichiers `.c` unitaires que les agents écrivent à la volée. Ils sont compilés en mémoire par TCC -- aucun `.so` n'est jamais écrit sur le disque.

## Structure

Chaque plugin inclut `tc_plugin.h` et exporte quatre choses :

```c
#include "tc_plugin.h"

const char *TC_PLUGIN_NAME = "weather";
const char *TC_PLUGIN_DESC = "Récupérer la météo pour une ville";
const char *TC_PLUGIN_SCHEMA =
    "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\",\"description\":\"Nom de la ville\"}},\"required\":[\"city\"]}";

const char *tc_execute(const char *input_json) {
    void *json = tc_json_parse(input_json);
    const char *city = tc_json_string(tc_json_get(json, "city"));
    if (!city)
        return "error: missing city";

    char url[256];
    tc_snprintf(url, sizeof(url), "https://wttr.in/%s?format=3", city);

    static char result[512];
    int status = tc_http_get(url, result, sizeof(result));
    return (status == 200) ? result : "error: weather service unavailable";
}
```

Le template builder est dans [`plugins/_template.c`](../plugins/_template.c). Les fichiers commençant par `_` sont ignorés par le scanner.

## Exécution

Les plugins sont compilés en mode `-nostdlib` : pas de libc, pas de headers système. Le daemon injecte un ensemble de fonctions `tc_*` via `tcc_add_symbol()` avant la compilation, ce qui donne aux plugins HTTP+TLS, JSON et I/O fichier. Les noms courants des fonctions de chaînes de la libc (`strlen`, `snprintf`, `strcat`, `malloc`...) sont des macros vers leur version `tc_*` ; `printf`, `sprintf` et `fopen` n'existent pas.

Chaque appel tourne dans un processus fils qui se termine ensuite, avec un délai maximal de 180 secondes :

- un plantage ou une boucle infinie fait échouer l'appel, pas le daemon ;
- la mémoire est libérée à la fin de l'appel, donc `tc_free()` et `tc_json_free()` sont facultatifs ;
- `tc_http_get()` suit les redirections et encode les espaces et les octets non ASCII de l'URL.

Ce n'est pas un bac à sable de sécurité : un plugin est du code natif qui tourne avec les droits du daemon.

## Noms

`TC_PLUGIN_NAME` devient un nom d'outil : lettres, chiffres, `_` et `-`, 64 caractères au plus, pas le nom d'un outil intégré, et pas déjà pris par un autre fichier de plugin. Un plugin qui enfreint ces règles est refusé, car un seul nom d'outil invalide ferait échouer tous les appels à l'API.

## Tests

`create_plugin` accepte un `test_input` optionnel (un objet JSON). Après la compilation, le daemon lance le plugin une fois avec cette entrée et renvoie la sortie avec la trace de chaque appel HTTP (URL, statut, début du corps). Un plantage, un dépassement de délai, une sortie vide ou une sortie qui commence par `error` font échouer `create_plugin`, pour que le builder corrige le code. Les erreurs de compilation reviennent avec la ligne de source de chaque erreur.

## Fonctions disponibles

| Catégorie | Fonctions |
|-----------|-----------|
| Mémoire | `tc_malloc`, `tc_free` |
| Chaînes | `tc_strlen`, `tc_strcmp`, `tc_strncmp`, `tc_strcpy`, `tc_strncpy`, `tc_strcat`, `tc_strncat`, `tc_strdup`, `tc_snprintf`, `tc_memcpy`, `tc_memset`, `tc_memcmp`, `tc_strstr`, `tc_strchr`, `tc_strrchr`, `tc_atoi` |
| Caractères | `tc_isdigit`, `tc_isalpha`, `tc_isspace`, `tc_tolower`, `tc_toupper` (ASCII) |
| Fichiers | `tc_read_file`, `tc_write_file` |
| HTTP | `tc_http_get`, `tc_http_post`, `tc_http_post_json`, `tc_http_header` |
| JSON | `tc_json_parse`, `tc_json_free`, `tc_json_print`, `tc_json_get`, `tc_json_index`, `tc_json_array_size`, `tc_json_string`, `tc_json_int`, `tc_json_double` |
| Système | `tc_gethostname` |
| Logging | `tc_log` |

Les appels HTTP passent par la stack BearSSL du daemon.

Voir [`include/tc_plugin.h`](../include/tc_plugin.h) pour les déclarations complètes.
