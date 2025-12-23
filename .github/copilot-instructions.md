# GitHub Copilot Instructions for trd_aioarb

## Repository Overview

This repository contains the **EZDG (EZ Dynamic Grid)** trading strategy implementation for the Mercury trading framework. It implements a sophisticated position-dependent dynamic grid trading strategy with intelligent risk management.

### Key Technologies
- **Language**: C++ (C++11 or later)
- **Framework**: Mercury Trading System
- **Domain**: Algorithmic trading, spread trading, grid strategies
- **Configuration**: XML-based

## Architecture

### Core Components

1. **Strategy Implementation** (`AioEZDG.cpp`)
   - Main strategy file containing grid logic
   - Position-dependent grid pricing algorithm
   - Integration with Mercury framework
   - ~4000+ lines of production trading code

2. **Entry Point Flow**
   ```
   trySignal() → updateSignal() → updtBuySell() → Grid Calculation
                                        ↓
                                 Set m_buy, m_sell
                                        ↓
                              Compare with market
                                        ↓
                               Generate signals
                                        ↓
                                Execute orders
   ```

3. **Key Classes**
   - `CStratsEnvAE`: Global strategy environment with grid parameters
   - `CSpreadExtentionAE`: Per-spread configuration and state
   - `CSpreadSignal`: Signal state with grid level persistence
   - `CSpreadExec`: Order execution engine

### Grid Strategy Logic

The core innovation is in the `updtBuySell()` function (lines 2683-2770):
- Calculates position-dependent grid levels
- Uses configurable `GridSpacing`, `GridAsymmetry`, and `UsePositionGrid` parameters
- Automatically adjusts to encourage position closing
- Persists grid state for recovery

## Coding Standards

### C++ Style Guidelines

1. **Naming Conventions**
   - Class names: `CamelCase` with `C` prefix (e.g., `CSpreadSignal`)
   - Member variables: `m_` prefix (e.g., `m_gridSpacing`, `m_refMid`)
   - Functions: `camelCase` (e.g., `updtBuySell`, `trySignal`)
   - Parameters: `camelCase` without prefix
   - Constants: Use existing Mercury framework conventions

2. **Code Structure**
   - Keep modifications minimal and surgical
   - Preserve existing Mercury framework patterns
   - Maintain 100% backward compatibility
   - Use existing logging patterns (e.g., `[finishComb]`, `SPRDTRD`)

3. **Comments**
   - Use comments sparingly, matching existing style
   - Document complex grid calculations
   - Explain non-obvious trading logic
   - No redundant comments for obvious code

4. **Error Handling**
   - Validate reference prices and positions
   - Check for division by zero in calculations
   - Use existing Mercury error handling patterns
   - Log errors using framework logging system

### Trading-Specific Conventions

1. **Grid Parameters**
   - `GridSpacing`: Grid width in ticks (default 1.0, range 0.5-5.0)
   - `GridAsymmetry`: Position closing urgency (default 0.5, range 0.0-1.0)
   - `GridWidth`: Reserved for future multi-level grids (default 5)
   - `UsePositionGrid`: Enable/disable feature (default 1)

2. **Position Management**
   - Respect `SprdMaxLot` position limits
   - Honor `MinAvailable` capital constraints
   - Use position-dependent pricing to manage risk
   - Maintain existing constraint system

3. **Price Calculations**
   - Always round to valid tick sizes
   - Use `m_tick` for tick size calculations
   - Validate against `m_refMid` (reference mid price)
   - Handle edge cases (zero position, invalid prices)

## Configuration

### XML Configuration Pattern

All strategy parameters are configured via XML:

```xml
<Strategy name="EZDG">
    <!-- Grid Parameters -->
    <Property name="GridSpacing" value="1.0"/>
    <Property name="GridAsymmetry" value="0.5"/>
    <Property name="UsePositionGrid" value="1"/>
    
    <!-- Spreads -->
    <ManSprds>
        <Spread name="cu2501-cu2502" 
                RefMid="100.0"
                SprdMaxLot="50"
                SprdStpLot="5"/>
    </ManSprds>
</Strategy>
```

**Important**: Never hardcode values that should be configurable via XML.

## Documentation

### Existing Documentation (MUST READ before changes)

1. **IMPLEMENTATION_SUMMARY.md** - Project overview, start here
2. **QUICK_START.md** - Setup and operation guide with examples
3. **GRID_STRATEGY_README.md** - Technical deep dive into grid algorithm
4. **GRID_VISUALIZATION.md** - Visual ASCII diagrams of grid behavior
5. **config_example.xml** - Complete configuration template with all parameters
6. **README.md** - Repository overview and quick reference

### Documentation Standards

- Keep documentation in sync with code changes
- Use clear examples with concrete numbers
- Include ASCII diagrams for visual clarity
- Document parameter ranges and effects
- Provide troubleshooting guidance

## Testing Guidelines

### Testing Approach

This is a production trading system. Testing must be done carefully:

1. **No Automated Unit Tests**
   - This repository does not have automated test infrastructure
   - Testing is done through backtesting and paper trading
   - Do not create test files or test frameworks

2. **Validation Methods**
   - Backtest with historical data first
   - Paper trade with small positions
   - Monitor logs for correct behavior
   - Validate grid calculations manually

3. **Key Validation Points**
   - Grid levels calculated correctly for different positions
   - XML parameters loaded and applied properly
   - State persists across restarts
   - Backward compatibility maintained (UsePositionGrid=0)

### Deployment Process

1. **Backtest Mode** - Test with historical data
2. **Paper Trading** - Small positions (SprdMaxLot=10)
3. **Live Trading** - Gradual increase to production limits

## What Copilot Should Do

### ✅ Good Tasks for Copilot

- **Documentation updates** - README, guides, examples
- **Configuration examples** - Adding new XML config snippets
- **Code comments** - Clarifying complex grid calculations
- **Parameter validation** - Adding range checks for config values
- **Logging improvements** - Enhanced monitoring output
- **Refactoring** - Improving code structure while preserving behavior
- **Bug fixes** - Clear, isolated issues with known solutions

### ❌ Tasks to Avoid

- **Core trading logic changes** - Requires deep domain expertise
- **Order execution changes** - Critical for production safety
- **Risk management modifications** - Needs human oversight
- **Framework integration changes** - Mercury framework is complex
- **Performance optimizations** - Trading systems require careful testing
- **Multi-file refactors** - High risk in production trading code

### ⚠️ Proceed with Caution

- **Grid algorithm changes** - Test extensively with backtesting
- **New parameters** - Ensure backward compatibility
- **State persistence changes** - Critical for recovery
- **Price calculations** - Must be exact for trading

## Critical Considerations

### Trading System Safety

1. **Backward Compatibility**
   - ALWAYS maintain compatibility with existing configurations
   - Use defaults that preserve current behavior
   - Provide rollback options (e.g., UsePositionGrid=0)

2. **Position Risk**
   - Never bypass position limit checks
   - Preserve existing risk management
   - Test position accumulation scenarios

3. **Price Accuracy**
   - All prices must round to valid tick sizes
   - Validate calculations against reference prices
   - Handle edge cases (zero position, invalid data)

4. **State Persistence**
   - Grid state must persist across restarts
   - Test recovery scenarios
   - Validate JSON serialization

### Code Change Guidelines

1. **Minimize Changes**
   - Make surgical, targeted modifications
   - Preserve existing code structure
   - Keep changes concentrated in few functions

2. **Preserve Patterns**
   - Follow existing Mercury framework patterns
   - Use established logging formats
   - Maintain consistent error handling

3. **Document Changes**
   - Update relevant documentation files
   - Add examples for new features
   - Explain configuration parameters

## Common Patterns

### Logging Patterns

```cpp
// Startup/Configuration
[finishComb],spread_name,gridSpacing,1.0,gridAsymmetry,0.5

// Runtime Grid Levels
gridBV,100,QuoteB,999.9,TheoB,999.8,TheoA,1000.2,QuoteA,1000.1,GridAV,100

// Trade Execution
SPRDTRD,sprd,spread_name,price,999.95,vlm,5
```

### XML Parameter Loading

```cpp
// Load from XML with default
m_gridSpacing = getDoubleProperty("GridSpacing", 1.0);
m_gridAsymmetry = getDoubleProperty("GridAsymmetry", 0.5);
m_usePositionGrid = getIntProperty("UsePositionGrid", 1);
```

### Grid Calculation Pattern

```cpp
// 1. Get current position
double currentPos = signal->getPosition();

// 2. Calculate normalized position
double normalizedPos = currentPos / m_sprdMaxLot;

// 3. Apply asymmetry
double positionBias = -normalizedPos * gridTick * m_gridAsymmetry;

// 4. Calculate adjusted levels
double adjustedMid = m_refMid + positionBias;
buy = adjustedMid - gridTick;
sell = adjustedMid + gridTick;

// 5. Round to tick size
buy = roundToTick(buy);
sell = roundToTick(sell);
```

## Support and References

- Primary maintainer: Check repository owner
- Mercury Framework: Proprietary trading framework (limited external docs)
- Configuration: See config_example.xml for all parameters
- Troubleshooting: See QUICK_START.md for common issues

## Version Information

- **Strategy Version**: 1.0
- **Status**: Production Ready
- **Last Major Update**: December 16, 2024
- **Lines of Code**: ~4000 lines (AioEZDG.cpp)
- **Documentation**: 44 KB across 5 files

---

**Remember**: This is production trading code. Safety, accuracy, and backward compatibility are paramount. When in doubt, ask for human review.
