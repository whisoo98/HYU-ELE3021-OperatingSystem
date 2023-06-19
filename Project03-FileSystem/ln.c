#include "types.h"
#include "stat.h"
#include "user.h"


// - `ln[-h][old][new]`를 입력하면, `new` file을 `old` file의 Hard Link file로 만들어야한다.
// - `ln[-s][old][new]`를 입력하면, `new` file을 `old` file의 Symbolic Link file로 만들어야한다.
int
main(int argc, char* argv[])
{
  if (argc != 4) {
    printf(2, "Usage: ln -cmd old new\n");
    exit();
  }
  char* cmd = argv[1];
  if (!strcmp(cmd, "-h")) {
    if (link(argv[2], argv[3]) < 0)
      printf(2, "hard link %s %s: failed\n", argv[2], argv[3]);
  }
  else if (!strcmp(cmd, "-s")) {
    if (symlink(argv[2], argv[3]) < 0)
      printf(2, "soft link %s %s: failed\n", argv[2], argv[3]);
  }
  else {
    printf(2, "Wrong link %s %s %s: failed\n", argv[1], argv[2], argv[3]);
  }
  exit();
}
