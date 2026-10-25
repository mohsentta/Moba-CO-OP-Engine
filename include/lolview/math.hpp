#pragma once

#include <cmath>
#include <cstddef>

namespace lolview {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }

inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    return len > 1e-8f ? v * (1.0f / len) : Vec3{0.0f, 0.0f, 0.0f};
}

// Column-major 4x4 matrix, laid out for OpenGL (m[column * 4 + row]).
struct Mat4 {
    float m[16]{};

    static Mat4 identity() {
        Mat4 out;
        out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.0f;
        return out;
    }

    const float* data() const noexcept { return m; }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 out;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            out.m[col * 4 + row] = sum;
        }
    }
    return out;
}

inline Mat4 perspective(float fov_y_radians, float aspect, float near_plane, float far_plane) {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 out;
    out.m[0] = f / aspect;
    out.m[5] = f;
    out.m[10] = (far_plane + near_plane) / (near_plane - far_plane);
    out.m[11] = -1.0f;
    out.m[14] = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
    return out;
}

inline Mat4 look_at(const Vec3& eye, const Vec3& center, const Vec3& up) {    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);

    Mat4 out = Mat4::identity();
    out.m[0] = s.x;  out.m[4] = s.y;  out.m[8] = s.z;
    out.m[1] = u.x;  out.m[5] = u.y;  out.m[9] = u.z;
    out.m[2] = -f.x; out.m[6] = -f.y; out.m[10] = -f.z;
    out.m[12] = -dot(s, eye);
    out.m[13] = -dot(u, eye);
    out.m[14] = dot(f, eye);
    return out;
}

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

inline Mat4 translation(const Vec3& t) {
    Mat4 out = Mat4::identity();
    out.m[12] = t.x;
    out.m[13] = t.y;
    out.m[14] = t.z;
    return out;
}

inline Mat4 scaling(const Vec3& s) {
    Mat4 out = Mat4::identity();
    out.m[0] = s.x;
    out.m[5] = s.y;
    out.m[10] = s.z;
    return out;
}

inline Quat normalize(const Quat& q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-8f) return Quat{};
    return Quat{q.x / len, q.y / len, q.z / len, q.w / len};
}

// Column-vector T * R * S, ready for gl_Position = M * p.
inline Mat4 trs_matrix(const Vec3& t, const Vec3& s, const Quat& q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float x2 = x + x, y2 = y + y, z2 = z + z;
    const float xx = x * x2, yy = y * y2, zz = z * z2;
    const float xy = x * y2, xz = x * z2, yz = y * z2;
    const float wx = w * x2, wy = w * y2, wz = w * z2;

    Mat4 out = Mat4::identity();
    out.m[0] = (1.0f - (yy + zz)) * s.x;
    out.m[1] = (xy + wz) * s.x;
    out.m[2] = (xz - wy) * s.x;
    out.m[4] = (xy - wz) * s.y;
    out.m[5] = (1.0f - (xx + zz)) * s.y;
    out.m[6] = (yz + wx) * s.y;
    out.m[8] = (xz + wy) * s.z;
    out.m[9] = (yz - wx) * s.z;
    out.m[10] = (1.0f - (xx + yy)) * s.z;
    out.m[12] = t.x;
    out.m[13] = t.y;
    out.m[14] = t.z;
    return out;
}

inline Mat4 inverse_affine(const Mat4& m) {
    const float a = m.m[0], b = m.m[1], c = m.m[2];
    const float d = m.m[4], e = m.m[5], f = m.m[6];
    const float g = m.m[8], h = m.m[9], i = m.m[10];

    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-12f) return Mat4::identity();
    const float inv = 1.0f / det;

    Mat4 out = Mat4::identity();
    out.m[0] = (e * i - f * h) * inv;
    out.m[1] = (c * h - b * i) * inv;
    out.m[2] = (b * f - c * e) * inv;
    out.m[4] = (f * g - d * i) * inv;
    out.m[5] = (a * i - c * g) * inv;
    out.m[6] = (c * d - a * f) * inv;
    out.m[8] = (d * h - e * g) * inv;
    out.m[9] = (b * g - a * h) * inv;
    out.m[10] = (a * e - b * d) * inv;

    const float tx = m.m[12], ty = m.m[13], tz = m.m[14];
    out.m[12] = -(tx * out.m[0] + ty * out.m[4] + tz * out.m[8]);
    out.m[13] = -(tx * out.m[1] + ty * out.m[5] + tz * out.m[9]);
    out.m[14] = -(tx * out.m[2] + ty * out.m[6] + tz * out.m[10]);
    return out;
}

}  // namespace lolview
