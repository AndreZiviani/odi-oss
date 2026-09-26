# Public references

Documents this code is written against. They are not stored here: the ITU
recommendations are free to download but their copyright page forbids
reproduction without permission, so the repository carries their identity
(number, edition, SHA-256) and a script that fetches them:

    sh tools/fetch-refs.sh        # downloads into refs/ (gitignored), checks SHA-256

Comments in the code cite these by id and clause, for example
`G.984.3 clause 10.2.4, Table 10-1`.

| id | document | edition | file in refs/ | SHA-256 |
|---|---|---|---|---|
| G.984.3 | ITU-T G.984.3, Gigabit-capable passive optical networks (G-PON): Transmission convergence layer specification | 01/2014 | g984.3-201401.pdf | 6124e0c8736e6a48bd7d1f9b84dc922e137edf5a76cddaf1131f1bf9d40585e4 |
| G.984.3 Amd1 | Amendment 1 to the above | 03/2020 | g984.3-202003-amd1.pdf | e48b4f192a7dbf54dd1e7b961c6aa60b7cf56efcff53063d3cc4fbe6c64cc50c |

Where each one comes from:
- ITU page: https://www.itu.int/rec/T-REC-G.984.3-201401-I/en and
  https://www.itu.int/rec/T-REC-G.984.3-202003-I!Amd1/en. The ITU download
  endpoint refuses non-browser clients, so the script fetches the same
  files from the Internet Archive copy of that endpoint; the SHA-256 check
  proves it is the same document.

What the code uses them for:
- G.984.3 clause 9: PLOAM message formats, both directions (odi_gpon_ploam).
- G.984.3 clause 10: ONU activation, states O1 to O7, timers TO1 and TO2
  (odi_gpon_fsm).
- G.984.3 clauses 12.2 to 12.4 and Amd1: encryption, key exchange and
  key switch-over; Annex A.2: AES golden vectors (host tests).

G.988 (OMCI) and SFF-8472 (optics DDM) are cited by clause in the code too
but are not fetched by the script. Register names in the drivers are our
own (`src/diag/tools/regnames.txt`); no register map is in this repository
(`docs/LICENSING.md`).
