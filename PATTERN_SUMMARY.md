# 六个模式计算总结

## 1. Triangle (三角形/3-Clique) 🔺

### 模式结构
```
    v0
   / \
  v1---v2
```
3个顶点形成的完全图，每个顶点都与其他两个顶点相连（共3条边）。

### 计算方式
```cpp
for (vidType v0 = 0; v0 < g.V(); v0++) {
  auto y0 = g.N(v0);
  auto y0f0 = bounded(y0, v0);  // 只包含 < v0 的邻居
  for (auto v1 : y0f0) {
    counter += intersection_num(y0f0, g.N(v1), v1);
  }
}
```

### 关键点
- **顶点扩展顺序**: v0 → v1 → v2
- **过滤条件**: v1 < v0, v2 < v1
- **交集操作**: 1次
  - `intersection_num(y0f0, g.N(v1), v1)` - 计算v2的数量
  - v2 ∈ N(v0) ∩ N(v1)，且 v2 < v1

---

## 2. Rectangle (矩形/4-Cycle) ⬜

### 模式结构
```
v0---v1
|     |
v2---v3
```
4个顶点形成的矩形（4-cycle），其中v0连接v1和v2，v1和v2都连接v3。

### 计算方式
```cpp
for (vidType v0 = 0; v0 < g.V(); v0++) {
  for (vidType v1 : g.N(v0)) {
    if (v1 >= v0) break;
    auto y1 = g.N(v1);
    for (vidType v2 : g.N(v0)) {
      if (v2 >= v1) break;
      counter += intersection_num(y1, g.N(v2), v0);
    }
  }
}
```

### 关键点
- **顶点扩展顺序**: v0 → v1 → v2 → v3
- **过滤条件**: v1 < v0, v2 < v1, v3 < v0
- **交集操作**: 1次
  - `intersection_num(y1, g.N(v2), v0)` - 计算v3的数量
  - v3 ∈ N(v1) ∩ N(v2)，且 v3 < v0

---

## 3. Diamond (钻石模式) 💎

### 模式结构
```
    v0
   / \
  v1---v2
   \ /
    v3
```
一个四边形，其中v0-v1-v2-v3形成钻石形状，v1和v2都连接v0和v3。

### 计算方式
```cpp
for (vidType v0 = 0; v0 < g.V(); v0++) {
  auto y0 = g.N(v0);
  for (vidType v1 : g.N(v0)) {
    if (v1 >= v0) break;
    auto y0y1 = intersection_set(y0, g.N(v1));
    for (vidType v2 : y0y1) {
      for (vidType v3 : y0y1) {
        if (v3 >= v2) break;
        counter += 1;
      }
    }
  }
}
```

### 关键点
- **顶点扩展顺序**: v0 → v1 → v2, v3
- **过滤条件**: v1 < v0, v2, v3 ∈ N(v0) ∩ N(v1), v3 < v2
- **交集操作**: 1次
  - `y0y1 = intersection_set(y0, g.N(v1))` - 找到v2和v3
  - v2和v3必须同时是v0和v1的邻居

---

## 4. Tailed Triangle (带尾三角形) 🔺

### 模式结构
```
    v0
   / \
  v1---v2
   |
   v3
```
一个三角形v0-v1-v2，加上一条尾巴v1-v3。

### 计算方式
```cpp
for (vidType v0 = 0; v0 < g.V(); v0++) {
  for (vidType v1 : g.N(v0)) {
    auto y0y1 = intersection_set(g.N(v0), g.N(v1), v1);
    for (vidType v2 : y0y1) {
      for (vidType v3 : g.N(v0)) {
        if (v3 == v1 || v3 == v2) continue;
        counter += 1;
      }
    }
  }
}
```

### 关键点
- **顶点扩展顺序**: v0 → v1 → v2 → v3
- **过滤条件**: v2 < v1, v3 ≠ v1, v3 ≠ v2
- **交集操作**: 1次
  - `y0y1 = intersection_set(g.N(v0), g.N(v1), v1)` - 找到v2
  - v2 ∈ N(v0) ∩ N(v1)，且 v2 < v1（形成三角形）
  - v3 ∈ N(v0)，但 v3 ≠ v1, v2（形成尾巴）

---

## 5. 4-Clique (4顶点完全图) 🔷

### 模式结构
```
    v0
   /|\
  v1-v2
   \|/
    v3
```
4个顶点形成的完全图，每个顶点都与其他所有顶点相连（共6条边）。

### 计算方式
```cpp
for (vidType v0 = 0; v0 < g.V(); v0++) {
  auto y0 = g.N(v0);
  for (auto v1 : y0) {
    if (v1 >= v0) break;
    auto y0y1 = y0 & g.N(v1);
    for (auto v2 : y0y1) {
      if (v2 >= v1) break;
      counter += intersection_num(y0y1, g.N(v2), v2);
    }
  }
}
```

### 关键点
- **顶点扩展顺序**: v0 → v1 → v2 → v3
- **过滤条件**: v1 < v0, v2 < v1, v3 < v2
- **交集操作**: 2次
  - 第1次: `y0y1 = y0 & g.N(v1)` - 找到v2（v2 ∈ N(v0) ∩ N(v1)）
  - 第2次: `intersection_num(y0y1, g.N(v2), v2)` - 计算v3的数量
  - v3 ∈ N(v0) ∩ N(v1) ∩ N(v2)，且 v3 < v2（三重交集）

---

## 6. 5-Clique (5顶点完全图) 🔶

### 模式结构
```
    v0
   /|\
  v1-v2-v3
   \|/|/
    v4
```
5个顶点形成的完全图，每个顶点都与其他所有顶点相连（共10条边）。

### 计算方式
```cpp
for (vidType v1 = 0; v1 < g.V(); v1++) {  // 注意：从v1开始
  uint64_t local_counter = 0;
  auto y1 = g.N(v1);
  for (auto v2 : y1) {
    if (v2 > v1) break;
    auto y1y2 = intersection_set(y1, g.N(v2));
    for (auto v3 : y1y2) {
      if (v3 > v2) break;
      auto y1y2y3 = intersection_set(y1y2, g.N(v3));
      for (auto v4 : y1y2y3) {
        if (v4 > v3) break;
        local_counter += intersection_num(y1y2y3, g.N(v4), v4);
      }
    }
  }
  counter += local_counter;
}
```

### 关键点
- **顶点扩展顺序**: v1 → v2 → v3 → v4 → v0（注意从v1开始）
- **过滤条件**: v2 < v1, v3 < v2, v4 < v3, v0 < v4
- **交集操作**: 3次
  - 第1次: `y1y2 = intersection_set(y1, g.N(v2))` - 找到v3
  - 第2次: `y1y2y3 = intersection_set(y1y2, g.N(v3))` - 找到v4
  - 第3次: `intersection_num(y1y2y3, g.N(v4), v4)` - 计算v0的数量
  - v0 ∈ N(v1) ∩ N(v2) ∩ N(v3) ∩ N(v4)，且 v0 < v4（四重交集）

---

## 对比总结表

| 模式 | 顶点数 | 边数 | 交集次数 | 最大交集深度 | 关键特征 |
|------|--------|------|---------|------------|---------|
| **Triangle** | 3 | 3 | 1 | 二重交集 | 最简单的完全图 |
| **Rectangle** | 4 | 4 | 1 | 二重交集 | 4-cycle，v3连接v1和v2 |
| **Diamond** | 4 | 5 | 1 | 二重交集 | v2和v3都是v0和v1的共同邻居 |
| **Tailed Triangle** | 4 | 4 | 1 | 二重交集 | 三角形+尾巴，v3只连接v0 |
| **4-Clique** | 4 | 6 | 2 | 三重交集 | 所有顶点两两相连 |
| **5-Clique** | 5 | 10 | 3 | 四重交集 | 所有顶点两两相连 |

## 交集操作复杂度对比

- **Triangle**: 最简单，只需要一次二重交集
- **Rectangle/Diamond/Tailed Triangle**: 需要一次二重交集，但结构不同
- **4-Clique**: 需要两次交集，第二次是三重交集
- **5-Clique**: 最复杂，需要三次交集，最后一次是四重交集

## 去重策略

所有模式都使用顶点ID排序来避免重复计数：
- Triangle: v1 < v0, v2 < v1
- Rectangle: v1 < v0, v2 < v1, v3 < v0
- Diamond: v1 < v0, v3 < v2
- Tailed Triangle: v2 < v1
- 4-Clique: v1 < v0, v2 < v1, v3 < v2
- 5-Clique: v2 < v1, v3 < v2, v4 < v3, v0 < v4

