/*
 * Stubs for some cli functions used by the test routines.
 * $Revision: 92103 $
 */
void tris_cli(int fd, const char *fmt, ...);
void tris_cli(int fd, const char *fmt, ...)
{
}

struct tris_cli_entry;

int tris_cli_register_multiple(struct tris_cli_entry *e, int len);
int tris_cli_register_multiple(struct tris_cli_entry *e, int len)
{
	return 0;
}
