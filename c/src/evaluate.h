/* spg evaluate - streaming guess-curve evaluator.
 *
 * Port of src/evaluate.py (Semantic PRINCE).
 *
 * Contract with the dispatcher in main.c:
 *   int rc = spg_evaluate(argc - 1, argv + 1);
 * i.e. argv[0] is the subcommand name ("evaluate"), argv[1] is the
 * test-set path, argv[2] (optional) is the guess limit.
 *
 * Usage: generator | spg evaluate test.txt [limit]
 */
#ifndef SPG_EVALUATE_H
#define SPG_EVALUATE_H

int spg_evaluate(int argc, char **argv);

#endif
