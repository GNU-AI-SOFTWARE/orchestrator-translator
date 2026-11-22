<!--
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Orchestrator Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# CHANGELOG — Orchestrator Translator

Format « Keep a Changelog », versionnage sémantique. Les contrats gelés
de `SPEC.md` ne changent jamais silencieusement : toute évolution passe
par un bump de `version`.

## [Unreleased]

### Corrigé

- **`src/neuron.c` — lecture hors limites dans `orc_neuron_parse_outputs`** :
  `strtol`/`strchr`/`strtod` scannaient au-delà de `text_len` (ASan
  `global-buffer-overflow` reproduit via `tests/test_contract.c`, dont le
  littéral d'état fait 82 octets mais recevait une longueur de 90). Tous les
  scans sont désormais bornés par `text_len` ; le test utilise
  `sizeof state_text - 1` pour que la longueur ne puisse plus dériver.
- **`src/contract.c` — `atoi()` sur une suite de chiffres contrôlée** :
  comportement indéfini en cas de dépassement (confirmé sous UBSan) ;
  remplacé par une conversion bornée saturante, mêmes sémantiques
  d'acceptation.
- **`src/storage_db.c` — écriture d'un octet hors limites en mode pipe** :
  le NUL était écrit en `resp[total + r]` ; quand la réponse remplissait
  exactement `ORC_DB_RESP_MAX`, un octet dépassait du buffer. La lecture
  garde désormais toujours un octet pour le NUL.
- **Injection JSON via `topology`** : les champs texte étaient interpolés
  tels quels (`%s`) dans les lignes d'instruction `/db`
  (`orc_db_write_run`/`orc_db_write_instance`) et dans le JSON de statut
  (`build_status`, `src/orchestrate.c`) ; une topologie contenant `"`
  produisait du JSON invalide des deux côtés. Ajout de `put_json_str` /
  `json_escape`, vérifié de bout en bout avec un faux `/db` `dbexec:`
  (JSON validé par `json.loads`).
- **`src/scheduler.c`** : la valeur de retour de
  `orc_storage_json_vector` était ignorée — un rendu en échec aurait utilisé
  un buffer de pile non initialisé ; repli sur `null` désormais.
