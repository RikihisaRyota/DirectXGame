#pragma once
#include "Vector3.h"
class mat4x4
{
public:
	float m[4][4];
public:

	mat4x4();
	mat4x4(
		float _11, float _12, float _13, float _14,
		float _21, float _22, float _23, float _24,
		float _31, float _32, float _33, float _34,
		float _41, float _42, float _43, float _44
	) {
		m[0][0] = _11, m[0][1] = _12, m[0][2] = _13, m[0][3] = _14;
		m[1][0] = _21, m[1][1] = _22, m[1][2] = _23, m[1][3] = _24;
		m[2][0] = _31, m[2][1] = _32, m[2][2] = _33, m[2][3] = _34;
		m[3][0] = _41, m[3][1] = _42, m[3][2] = _43, m[3][3] = _44;
	}
};

//1,行列の加法
mat4x4 Add(const mat4x4& m1, const mat4x4& m2);
//2,行列の減算
mat4x4 Sub(const mat4x4& m1, const mat4x4& m2);
//3,行列の積
mat4x4 Mul(const mat4x4& m1, const mat4x4& m2);
//3,行列の積(スカラー倍)
mat4x4 Mul(const float scaler, const mat4x4& m2);
//4,逆行列
mat4x4 Inverse(const mat4x4& m);
//5,転置行列
mat4x4 Transpose(const mat4x4& m);
//6,単位行列
mat4x4 MakeIdentity4x4();

//1,平行移動行列
mat4x4 MakeTranslateMatrix(const Vector3& translate);
//2,拡大縮小行列
mat4x4 MakeScaleMatrix(const Vector3& scale);
//3,座標変換
Vector3 Transform(const Vector3& vector, const mat4x4& matrix);

//1,X軸回転行列
mat4x4 MakeRotateXMatrix(float radian);
//2,Y軸回転行列
mat4x4 MakeRotateYMatrix(float radian);
//3,Z軸回転行列
mat4x4 MakeRotateZMatrix(float radian);

//3次元アフィン変換
mat4x4 MakeAffineMatrix(const Vector3& scale, const Vector3& rotate, const Vector3& translate);

//1,透視投影行列
mat4x4 MakePerspectiveFovMatrix(float fovY, float aspectRatio, float nearClip, float farClip);
//2,正射影行列
mat4x4 MakeOrthographicMatrix(float left, float top, float right, float bottom, float nearClip, float farClip);
//3,ビューポート変換行列
mat4x4 MakeViewportMatrix(float left, float top, float width, float height, float minDepth, float maxDepth);

//matrix4x4*Vector3
Vector3 operator* (const mat4x4& mat, const Vector3& v);