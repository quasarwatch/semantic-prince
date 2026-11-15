/* spg targeted - targeted attack mode (port of src/targeted.py).
 *
 * Contract with the dispatcher in main.c (same as cmd_pairs/cmd_chains):
 *   int rc = spg_targeted(argc - 2, argv + 2);
 * i.e. argv[0..] are the original command line arguments of targeted.py:
 *   spg targeted UNI BI TEST [NP] [K] [--service=NAME] [--personal=FILE]
 *                [--deleet] [--emit[=w1,w2,...]]
 */
#ifndef SPG_TARGETED_H
#define SPG_TARGETED_H

int spg_targeted(int argc, char **argv);

#endif
