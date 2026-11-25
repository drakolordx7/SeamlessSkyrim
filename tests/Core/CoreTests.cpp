#include <cstdio>

#include "Core/Math/Transform.h"

int main()
{
	const Core::Transform t;
	std::printf("%f\n", (t * Core::Vec3{ 1, 2, 3 }).x);
	return 0;
}
