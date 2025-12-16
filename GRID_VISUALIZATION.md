# Grid Strategy Visual Reference

## Grid Behavior Visualization

### Scenario 1: FLAT Position (pos = 0)

```
Price
  ^
  |
1000.50 |
1000.40 |
1000.30 |
1000.20 |
1000.10 | -------- SELL GRID LEVEL -------- [Symmetric]
1000.00 | ======== REFERENCE MID ==========
 999.90 | -------- BUY GRID LEVEL --------- [Symmetric]
 999.80 |
 999.70 |
 999.60 |
 999.50 |
        +---> Time

Grid Spacing: 1.0 tick ($0.10)
Position: 0 (FLAT)
Behavior: Symmetric grid - normal market making
```

### Scenario 2: LONG Position (pos = +10)

```
Price
  ^
  |
1000.50 |
1000.40 |
1000.30 |
1000.20 |
1000.10 |
1000.00 | ======== ORIGINAL REFERENCE MID ==
 999.95 | -------- SELL GRID (tighter) ----- [Encourage close]
 999.90 | -------- Adjusted Mid (shifted down)
 999.80 |
 999.70 | -------- BUY GRID (wider) --------- [Discourage open]
 999.60 |
 999.50 |
        +---> Time

Grid Spacing: 1.0 tick ($0.10)
Position: +10 lots (LONG)
Grid Asymmetry: 0.5
Behavior: Grid shifts DOWN to encourage selling
```

### Scenario 3: SHORT Position (pos = -10)

```
Price
  ^
  |
1000.50 |
1000.40 |
1000.30 | -------- SELL GRID (wider) -------- [Discourage open]
1000.20 |
1000.10 | -------- Adjusted Mid (shifted up)
1000.05 | -------- BUY GRID (tighter) -------- [Encourage close]
1000.00 | ======== ORIGINAL REFERENCE MID ====
 999.90 |
 999.80 |
 999.70 |
 999.60 |
 999.50 |
        +---> Time

Grid Spacing: 1.0 tick ($0.10)
Position: -10 lots (SHORT)
Grid Asymmetry: 0.5
Behavior: Grid shifts UP to encourage buying
```

## Parameter Effects

### GridSpacing Impact

```
GridSpacing = 0.5 (TIGHT)
------------------------------
1000.05 | -------- SELL --------
1000.00 | ======== MID ==========
 999.95 | -------- BUY ---------

GridSpacing = 1.0 (NORMAL)
------------------------------
1000.10 | -------- SELL --------
1000.00 | ======== MID ==========
 999.90 | -------- BUY ---------

GridSpacing = 2.0 (WIDE)
------------------------------
1000.20 | -------- SELL --------
1000.00 | ======== MID ==========
 999.80 | -------- BUY ---------

Effect: Larger spacing = Less frequent trades
```

### GridAsymmetry Impact (with pos = +10)

```
GridAsymmetry = 0.0 (NO ASYMMETRY)
-------------------------------------
1000.10 | -------- SELL -------- [symmetric]
1000.00 | ======== MID ==========
 999.90 | -------- BUY --------- [symmetric]

GridAsymmetry = 0.5 (MODERATE)
-------------------------------------
 999.95 | -------- SELL -------- [tighter]
 999.90 | -------- Adjusted Mid -
 999.70 | -------- BUY --------- [wider]

GridAsymmetry = 1.0 (AGGRESSIVE)
-------------------------------------
 999.90 | -------- SELL -------- [very tight]
 999.80 | -------- Adjusted Mid -
 999.50 | -------- BUY --------- [very wide]

Effect: Larger asymmetry = More aggressive position closing
```

## Trade Execution Flow

```
Market Data Update
        |
        v
+------------------+
| trySignal()      |
+------------------+
        |
        v
+------------------+
| updateSignal()   |
+------------------+
        |
        v
+------------------+
| updtBuySell()    |  <--- GRID LOGIC HERE
+------------------+
        |
        v
    [Calculate]
    - Get position
    - Calculate bias
    - Adjust mid
    - Calculate levels
    - Round to tick
        |
        v
+------------------+
| m_buy, m_sell    |  <--- Grid levels set
+------------------+
        |
        v
    [Compare with market]
    m_spBP >= m_sell ? SELL
    m_spAP <= m_buy  ? BUY
        |
        v
+------------------+
| Generate Signal  |
+------------------+
        |
        v
+------------------+
| Execute Orders   |
+------------------+
```

## Grid State Machine

```
+-------------+
|   FLAT      |
| (pos = 0)   |
+-------------+
      |
      | BUY executed
      v
+-------------+     +-------------+
|   LONG      | <-> |   VERY      |
| (pos = +N)  |     | LONG        |
+-------------+     | (pos = +2N) |
      |             +-------------+
      | SELL executed     |
      v                   | SELL executed
+-------------+           |
|   FLAT      | <---------+
| (pos = 0)   |
+-------------+
      |
      | SELL executed
      v
+-------------+     +-------------+
|   SHORT     | <-> |   VERY      |
| (pos = -N)  |     | SHORT       |
+-------------+     | (pos = -2N) |
      |             +-------------+
      | BUY executed      |
      v                   | BUY executed
+-------------+           |
|   FLAT      | <---------+
| (pos = 0)   |
+-------------+

Grid adjusts at each state transition:
- Moving away from flat: Grid widens in accumulation direction
- Moving toward flat: Grid tightens in closing direction
```

## Price Level Decision Tree

```
                    Market Update
                         |
                         v
                 +---------------+
                 | Get Position  |
                 +---------------+
                         |
        +----------------+----------------+
        |                |                |
        v                v                v
   pos = 0          pos > 0          pos < 0
   (FLAT)           (LONG)           (SHORT)
        |                |                |
        v                v                v
  Symmetric        Shift DOWN       Shift UP
   Grid            Grid              Grid
        |                |                |
        v                v                v
  Buy = Mid-1      Buy = Mid-1.5    Buy = Mid-0.5
  Sell = Mid+1     Sell = Mid-0.5   Sell = Mid+1.5
        |                |                |
        +----------------+----------------+
                         |
                         v
                 +---------------+
                 | Round to Tick |
                 +---------------+
                         |
                         v
                 +---------------+
                 | Set m_buy,    |
                 |     m_sell    |
                 +---------------+
                         |
                         v
                 +---------------+
                 | Compare with  |
                 | Market Prices |
                 +---------------+
                         |
        +----------------+----------------+
        |                                 |
        v                                 v
  m_spBP >= m_sell                  m_spAP <= m_buy
        |                                 |
        v                                 v
  Generate SELL                     Generate BUY
     Signal                            Signal
```

## Configuration Impact Matrix

| GridSpacing | GridAsymmetry | UsePositionGrid | Result                          |
|-------------|---------------|-----------------|----------------------------------|
| 1.0         | 0.5           | 1               | **RECOMMENDED: Balanced**       |
| 0.5         | 0.5           | 1               | Frequent trades, balanced risk  |
| 2.0         | 0.5           | 1               | Less frequent, balanced risk    |
| 1.0         | 0.0           | 1               | Balanced frequency, no closing  |
| 1.0         | 1.0           | 1               | Balanced frequency, fast close  |
| 1.0         | 0.5           | 0               | Balanced frequency, symmetric   |
| 0.5         | 1.0           | 1               | High frequency, aggressive close|
| 2.0         | 0.0           | 1               | Low frequency, slow close       |

## Position Accumulation Example

```
Time    Position    Buy Grid    Sell Grid    Action Taken
------------------------------------------------------------
09:00   0          999.90      1000.10       [Wait]
09:05   0          999.90      1000.10       Buy @ 999.90
09:10   +5         999.70      1000.00       [Wait]
09:15   +5         999.70      1000.00       Buy @ 999.70
09:20   +10        999.50      999.95        [Wait]
09:25   +10        999.50      999.95        Sell @ 999.95
09:30   +5         999.70      1000.00       [Wait]
09:35   +5         999.70      1000.00       Sell @ 1000.00
09:40   0          999.90      1000.10       [Back to Flat]

Notice:
- Grid widens as position accumulates (999.90 → 999.70 → 999.50)
- Sell grid tightens to encourage closing (1000.10 → 1000.00 → 999.95)
- Grid returns to symmetric when flat
```

## Risk Management Visualization

```
Maximum Position Limit
           |
           v
    +-------------+
    |  Grid Auto  |
    |  Widens on  |
    |  Buy Side   |
    +-------------+
           |
           v
[Discourages further accumulation]
           |
           v
    +-------------+
    |  Grid Auto  |
    |  Tightens   |
    |  on Sell    |
    +-------------+
           |
           v
[Encourages position reduction]
           |
           v
Position approaches zero
```

## Summary

The dynamic grid provides automatic risk management through:

1. **Position Awareness**: Grid knows current exposure
2. **Automatic Adjustment**: No manual intervention needed
3. **Risk Reduction**: Encourages closing as position grows
4. **Configurable**: Tune aggressiveness via parameters
5. **Reversible**: Grid returns to symmetric when flat

This creates a self-regulating system that naturally manages risk while maintaining market-making capability.
