#include "torne_core.h"
#include "torne_dib.h"

static int control(void *, u8, u16, u16, void *, u16)
{
	return -1;
}

int main()
{
	torne_ops ops{};
	ops.control = control;
	torne_bridge bridge{};
	torne_dib dib{};
	torne_bridge_init(&bridge, &ops, nullptr);
	torne_dib_setup(&dib, &bridge);
	return 0;
}
