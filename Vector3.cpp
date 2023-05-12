#include "Vector3.h"

float Dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vector3 Cross(const Vector3& a, const Vector3& b) {
	return { a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x };
}

Vector3 operator-(const Vector3& v, const Vector3& v2) {
	Vector3 tmp(0.0f, 0.0f, 0.0f);
	tmp.x = v.x - v2.x;
	tmp.y = v.y - v2.y;
	tmp.z = v.z - v2.z;
	return tmp;
}

Vector3 operator*(const Vector3& v, const Vector3& v2) {
	Vector3 tmp(0.0f, 0.0f, 0.0f);
	tmp.x = v.x * v2.x;
	tmp.y = v.y * v2.y;
	tmp.z = v.z * v2.z;
	return tmp;
}
