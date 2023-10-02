#pragma once
#include "Vector3.h"
#include "mat4x4.h"
struct WorldTransform {
	Vector3 scale;
	Vector3 rotate;
	Vector3 translate;
	mat4x4 WVP;
	mat4x4 World;
};