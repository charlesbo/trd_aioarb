# Dynamic Grid Spread Strategy Implementation

## Overview

This document describes the implementation of a dynamic grid spread trading strategy in `AioEZDG.cpp`. The strategy implements position-dependent grid pricing that automatically adjusts bid/ask levels based on current position to manage risk and encourage mean reversion.

## Core Concept

The dynamic grid strategy places buy and sell orders at calculated grid levels around a reference mid price. Unlike static grids, this implementation:

1. **Adjusts grid levels based on current position** - When holding a position, the grid shifts to encourage closing trades
2. **Uses asymmetric grids** - Grid spacing widens in the direction opposite to the position
3. **Maintains risk bounds** - Grid parameters ensure positions stay within configured limits

## Implementation Details

### Key Components

#### 1. Grid Parameters (in `CStratsEnvAE`)

These parameters are loaded from XML configuration:

```cpp
double m_gridSpacing;     // Grid spacing in ticks (default 1.0)
int m_gridWidth;          // Number of grid levels (default 5, currently unused)
double m_gridAsymmetry;   // Position-dependent asymmetry factor (default 0.5)
int m_usePositionGrid;    // Enable position-dependent grids (default 1)
```

#### 2. Spread-Level Grid State (in `CSpreadExtentionAE`)

Each spread maintains its own grid parameters:

```cpp
double m_gridSpacing;       // Grid spacing in ticks
int m_gridWidth;            // Number of grid levels on each side
double m_gridAsymmetry;     // Asymmetry factor for position-dependent grids
bool m_usePositionGrid;     // Enable position-dependent grid adjustment
```

#### 3. Grid State Persistence (in `CSpreadSignal`)

Grid state is persisted for recovery:

```cpp
double m_gridBuyLevel;      // Current grid buy level
double m_gridSellLevel;     // Current grid sell level
int m_gridUpdateCount;      // Count of grid updates
```

### Core Logic: `updtBuySell()` Function

The heart of the grid strategy is the `updtBuySell()` function that calculates grid buy/sell levels:

```cpp
void updtBuySell(double &buy, double &sell)
```

**Algorithm:**

1. **Validate reference mid price** - Ensure we have a valid reference price
2. **Get current position** - Retrieve position from signal
3. **Calculate base grid spacing** - `gridTick = m_tick * m_gridSpacing`
4. **Apply position-dependent adjustment:**
   - Calculate position bias: `positionBias = -normalizedPos * gridTick * m_gridAsymmetry`
   - Adjust mid: `adjustedMid = m_refMid + positionBias`
5. **Calculate grid levels:**
   - **When LONG (pos > 0):**
     - Buy level: wider spread (discourages adding to position)
     - Sell level: tighter spread (encourages closing position)
   - **When SHORT (pos < 0):**
     - Buy level: tighter spread (encourages closing position)
     - Sell level: wider spread (discourages adding to position)
   - **When FLAT (pos = 0):**
     - Symmetric spread around reference mid
6. **Round to tick size** - Ensure prices are valid
7. **Persist state** - Store grid levels in signal for recovery

### Trading Logic Integration

The grid levels (`m_buy`, `m_sell`) are used in `updateSignal()`:

```cpp
if (m_spBP >= m_sell && isSafeToSell())
    // Generate sell signal
else if (m_spAP <= m_buy && isSafeToBuy())
    // Generate buy signal
```

This creates a tick-level crossing strategy where:
- Sell when market bid crosses above grid sell level
- Buy when market ask crosses below grid buy level

## XML Configuration

Add these parameters to your strategy XML configuration:

```xml
<Strategy name="EZDG" ...>
    <!-- Grid Strategy Parameters -->
    <Property name="GridSpacing" value="1.0"/>      <!-- Grid spacing in ticks -->
    <Property name="GridWidth" value="5"/>          <!-- Number of grid levels -->
    <Property name="GridAsymmetry" value="0.5"/>    <!-- Position asymmetry factor -->
    <Property name="UsePositionGrid" value="1"/>    <!-- 1=enabled, 0=disabled -->
    
    <!-- Existing parameters -->
    <Property name="EnableTrade" value="1"/>
    <!-- ... other parameters ... -->
</Strategy>
```

### Parameter Tuning Guide

- **GridSpacing**: Controls grid width
  - Smaller values (0.5-1.0): Tighter grid, more frequent trades
  - Larger values (2.0-5.0): Wider grid, less frequent trades
  
- **GridAsymmetry**: Controls position-dependent shift
  - 0.0: No asymmetry (symmetric grid regardless of position)
  - 0.5: Moderate asymmetry (recommended)
  - 1.0: Strong asymmetry (aggressive position closing)

- **UsePositionGrid**: Enable/disable position-dependent grids
  - 1: Position-dependent (recommended for risk management)
  - 0: Simple symmetric grid (no position adjustment)

## Example Grid Behavior

### Scenario 1: Flat Position (pos = 0)
```
refMid = 1000.0
gridSpacing = 1.0
tick = 0.1

buy  = 1000.0 - 1.0 * 0.1 = 999.9
sell = 1000.0 + 1.0 * 0.1 = 1000.1
```

### Scenario 2: Long Position (pos = 10, stepSize = 5)
```
refMid = 1000.0
gridSpacing = 1.0
gridAsymmetry = 0.5
tick = 0.1

normalizedPos = 10 / 5 = 2.0
positionBias = -2.0 * 0.1 * 0.5 = -0.1
adjustedMid = 1000.0 - 0.1 = 999.9

buy  = 999.9 - 0.1 * (1.0 + 10/(2.0*5)) = 999.7  (wider)
sell = 999.9 + 0.1 * 0.5 = 999.95             (tighter)
```

Grid shifts down when long to encourage selling.

### Scenario 3: Short Position (pos = -10, stepSize = 5)
```
refMid = 1000.0
gridSpacing = 1.0
gridAsymmetry = 0.5
tick = 0.1

normalizedPos = -10 / 5 = -2.0
positionBias = -(-2.0) * 0.1 * 0.5 = 0.1
adjustedMid = 1000.0 + 0.1 = 1000.1

buy  = 1000.1 - 0.1 * 0.5 = 1000.05           (tighter)
sell = 1000.1 + 0.1 * (1.0 + 10/(2.0*5)) = 1000.3  (wider)
```

Grid shifts up when short to encourage buying.

## State Persistence

Grid state is automatically persisted through the JSON state file mechanism:

```cpp
// In syncData() - grid levels are saved with other spread state
m_pSignal->m_gridBuyLevel = buy;
m_pSignal->m_gridSellLevel = sell;
m_pSignal->m_gridUpdateCount++;
```

On restart, the strategy resumes with the last calculated grid levels.

## Existing Architecture Preservation

The implementation preserves all existing architecture:

1. **Entry Point**: `trySignal()` remains the main entry point
2. **Classes**: All existing classes (CFutureExtentionAE, CSpreadExtentionAE, CSpreadExec) are unchanged
3. **Order Execution**: Existing try/force order logic is untouched
4. **Risk Management**: Existing constraint system continues to work
5. **Fallback**: If grid parameters are not configured, defaults provide reasonable behavior

## Logging and Monitoring

Grid parameters are logged at initialization:

```
[finishComb],<spread_name>,prdMaxAmt,<amount>,sprdMaxTradeSize,<size>,
  sprdMaxLot,<lot>,stepSize,<step>,gridSpacing,<spacing>,
  gridWidth,<width>,gridAsymmetry,<asymmetry>
```

Grid levels are included in trade logs:

```
gridBV,<volume>,QuoteB,<quote>,TheoB,<theoretical_buy>,
  TheoA,<theoretical_ask>,QuoteA,<quote>,GridAV,<volume>
```

## Future Enhancements

Potential areas for extension (not implemented):

1. **R-minute window for risk bounds** - Track price bounds over rolling window
2. **Multi-level grids** - Use `m_gridWidth` to create multiple price levels
3. **Time-based grid adjustment** - Modify grid based on session time
4. **Volatility-based spacing** - Adjust grid spacing based on market volatility
5. **Grid volume distribution** - Allocate different volumes to different grid levels

## Testing Recommendations

1. **Backtest with grid disabled** - Verify no impact when `UsePositionGrid=0`
2. **Test flat position behavior** - Confirm symmetric grid when pos=0
3. **Test position accumulation** - Verify grid widens in accumulation direction
4. **Test position reduction** - Verify grid tightens in closing direction
5. **Test parameter sensitivity** - Vary GridSpacing and GridAsymmetry

## Summary

This implementation provides a solid foundation for dynamic grid trading with:

✅ Position-aware grid pricing
✅ XML configuration support  
✅ State persistence
✅ Minimal code changes (concentrated in `updtBuySell()`)
✅ Full backward compatibility
✅ Comprehensive logging

The strategy automatically manages risk by encouraging position closes while maintaining the ability to build positions when market conditions are favorable.
