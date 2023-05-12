#pragma once

/// <summary>
/// 3次元ベクトル
/// </summary>
struct Vector3 final {
	float x;
	float y;
	float z;
};
//内積
float Dot(const Vector3& a, const Vector3& b);
//クロス積
Vector3 Cross(const Vector3& v1, const Vector3& v2);

//-オーバーロード
Vector3 operator-(const Vector3& v, const Vector3& v2);

//-オーバーロード
Vector3 operator*(const Vector3& v, const Vector3& v2);