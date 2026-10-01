# Operating-System

This repository contains coursework solutions for operating systems assignments.

## Repository structure

- `assignment1/`
  - `soln.c`: Assignment 1 solution (custom shell implementation).
  - `statement.pdf`: Assignment handout.
- `assignment2/`
  - `soln.c`: Assignment 2 solution (MLFQ-style scheduler simulation).
  - `workload.c`: Workload program used by the scheduler.
  - `statement.pdf`: Assignment handout.

## Build

From the repository root:

### Assignment 1

```bash
gcc -std=c11 -Wall -Wextra -O2 assignment1/soln.c -o assignment1/shell
```

### Assignment 2

```bash
gcc -std=c11 -Wall -Wextra -O2 assignment2/workload.c -o assignment2/workload
gcc -std=gnu11 -Wall -Wextra -O2 assignment2/soln.c -o assignment2/scheduler
```

## Run

### Assignment 1 shell

```bash
./assignment1/shell
```

### Assignment 2 scheduler

```bash
cd assignment2
./scheduler <input_file>
```

Replace `<input_file>` with an input file following the Assignment 2 format from `statement.pdf`.