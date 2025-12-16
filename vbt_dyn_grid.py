import pandas as pd
import numpy as np
import vectorbt as vbt
import matplotlib.pyplot as plt
from numba import njit
from numba.typed import List
from numba.types import unicode_type
import itertools
import multiprocessing
from pathlib import Path
from datetime import timedelta
pd.set_option('display.max_rows', None)
vbt.settings['returns']['year_freq'] = f'{242*4*60}T'
# Specify the parameters here
init_cash = 400
vbt_init_cash = 50000
fee = 0.00005
leverage = 20
base_prd = 'T'
hedge_prd = 'TL'
base_size = 3.0 # 每层开仓 base_prd 数量
hedge_size = 1.0 # 每层对冲 hedge_prd 数量
# Parameters (replace with your desired values)
max_levels = 500 # 最大网格层级，防止无限开仓
glb_max_sets = 50
M = 10000 # 价差中的产品乘数
R = 15 # 更新间隔（分钟）
minutes_per_day = 255
print_details = False
bs_pth = Path('/home/mengchao/projs/trd_aioarb/ezcana/marb_TTL')
data_fn = bs_pth / f'data_1min_{base_prd}-{hedge_prd}.csv'
print(data_fn.absolute())
sd = '2023-04-01'
ed = '2025-12-31'
data = pd.read_csv(data_fn, index_col='date', parse_dates=True)
data = data[(data.index > sd) & (data.index < ed)]
price_base = data[base_prd]
price_hedge = data[hedge_prd]
print(f'data loaded from {data_fn}')
# Compute spread
spread = base_size * price_base - hedge_size * price_hedge
spread.plot()
# Convert to NumPy arrays for faster access
spread_values = spread.values.astype(np.float64)
price_base_values = price_base.values.astype(np.float64)
price_hedge_values = price_hedge.values.astype(np.float64)
# Create price DataFrame
price = pd.DataFrame({base_prd: price_base, hedge_prd: price_hedge})
price.plot()
# Calculate base_min_entry_interval as average n-minute range (assume n=60 for hourly avg range)
n_minutes = 60 # 可以调整n值，例如1440为日平均幅度
spread_nmin = spread.resample(f'{n_minutes}T').agg(['min', 'max'])
avg_range = (spread_nmin['max'] - spread_nmin['min']).mean()
base_min_entry_interval = avg_range if not np.isnan(avg_range) else 0.05 # 防NaN fallback
# Calculate base_exit_interval similarly
n_minutes = 240 # 可以调整n值，例如1440为日平均幅度
spread_nmin = spread.resample(f'{n_minutes}T').agg(['min', 'max'])
avg_range = (spread_nmin['max'] - spread_nmin['min']).mean()
base_exit_interval = avg_range if not np.isnan(avg_range) else 0.2 # Use same avg_range as base
# Precompute interval min and max (shared across all N)
interval_min = spread.resample(f'{R}T').min().dropna()
interval_max = spread.resample(f'{R}T').max().dropna()
# Precompute daily_range for avg_max_sets calculation
daily_range = interval_max.resample('D').max() - interval_min.resample('D').min()
# 新参数：风控
reduce_ratio = 0.6 # 减少亏钱腿持仓比例
rebound_amount = 3 * base_exit_interval # 回弹幅度（可调整）
base_is_integer = True
hedge_is_integer = True
@njit
def int_to_str(n):
    if n == 0:
        return '0'
    digits = List.empty_list(unicode_type)
    while n > 0:
        digits.append(chr(ord('0') + (n % 10)))
        n = n // 10
    # reverse
    rev = List.empty_list(unicode_type)
    for i in range(len(digits)-1, -1, -1):
        rev.append(digits[i])
    return ''.join(rev)
@njit
def float_to_str(f, prec=6):
    if np.isnan(f):
        return 'nan'
    if np.isinf(f):
        return 'inf' if f > 0 else '-inf'
    sign = '' if f >= 0 else '-'
    f = abs(f)
    scale = 10 ** prec
    i = int(f * scale + 0.5) # round
    s = int_to_str(i)
    if len(s) < prec + 1:
        s = '0' * (prec + 1 - len(s)) + s
    dot_pos = len(s) - prec
    s = s[:dot_pos] + '.' + s[dot_pos:]
    # remove trailing zeros and dot
    while s[-1] == '0':
        s = s[:-1]
    if s[-1] == '.':
        s = s[:-1]
    return sign + s
@njit
def get_label(pre_pos, order_size):
    if order_size > 0.0:
        if pre_pos >= 0.0:
            return '开仓 long'
        else:
            return '平仓 short'
    elif order_size < 0.0:
        if pre_pos > 0.0:
            return '平仓 long'
        else:
            return '开仓 short'
    else:
        return '无操作'
@njit
def simulate(spread_values, price_base_values, price_hedge_values, exit_interval, base_size, hedge_size, arbitrage_lower_values, arbitrage_upper_values, risk_lower_values, risk_upper_values, min_entry_interval, reduce_ratio, rebound_amount, timestamps, log_enabled, base_is_integer, hedge_is_integer, init_cash, fee, leverage):
    rebound_amount = exit_interval
    n = len(spread_values)
    orders_base = np.zeros(n, dtype=np.float64)
    orders_hedge = np.zeros(n, dtype=np.float64)
    curr_lvl = np.zeros(n, dtype=np.float64)
    num_opens = 0
    num_closes = 0
    max_open = 0
    logs = List.empty_list(unicode_type)
    # Dynamic grid adjustment parameters
    dynamic_factor_long = 1.0
    dynamic_factor_short = 1.0
    min_dynamic = 1.0
    max_dynamic = 3.0
    widen_threshold = 0.3
    narrow_threshold = 0.7
    widen_step = 1.2
    min_ops = 10
    num_opens_long = 0
    num_opens_short = 0
    num_closes_long = 0
    num_closes_short = 0
    profitable_closes_long = 0
    profitable_closes_short = 0
    profitable_rate_long_arr = np.full(n, np.nan, dtype=np.float64)
    profitable_rate_short_arr = np.full(n, np.nan, dtype=np.float64)
    entry_interval_long_arr = np.full(n, np.nan, dtype=np.float64)
    entry_interval_short_arr = np.full(n, np.nan, dtype=np.float64)
    current_max_sets_arr = np.full(n, np.nan, dtype=np.float64)
    current_cash_arr = np.full(n, np.nan, dtype=np.float64)
    # Initial calculation
    bound_distance = arbitrage_upper_values[0] - arbitrage_lower_values[0] # Use initial from passed values
    current_cash = init_cash
    equity_per_set = base_size * price_base_values[0] + hedge_size * price_hedge_values[0]
    current_max_sets = min(glb_max_sets, int(current_cash * leverage / equity_per_set) if equity_per_set > 0 else 0)
    entry_interval = (bound_distance / 2) / current_max_sets if current_max_sets > 0 else 0.0
    entry_interval = max(entry_interval, min_entry_interval) # 应用最小加仓间隔
    center = (arbitrage_lower_values[0] + arbitrage_upper_values[0]) / 2
    center_long = center - exit_interval / 2
    center_short = center + exit_interval / 2
    entry_interval_long = entry_interval * dynamic_factor_long
    entry_interval_short = entry_interval * dynamic_factor_short
    long_grids = np.array([center_long - k * entry_interval_long for k in range(1, max_levels + 1)], dtype=np.float64)
    short_grids = np.array([center_short + k * entry_interval_short for k in range(1, max_levels + 1)], dtype=np.float64)
    prev_lower = arbitrage_lower_values[0]
    prev_upper = arbitrage_upper_values[0]
    open_positions = List()
    open_positions.append((0.0, 0, 0.0))
    open_positions.pop()
    arbitrage_base_pos = 0.0
    arbitrage_hedge_pos = 0.0
    risk_base_pos = 0.0
    risk_hedge_pos = 0.0
    current_base_pos = 0.0
    current_hedge_pos = 0.0
    # 风控变量
    in_risk_mode = False
    break_direction = 0 # 1: upper break, -1: lower break
    max_sp = 0.0
    min_sp = 0.0
    reduced_leg = ''
    reduced_amount = 0.0
    reduced_direction = 0
    max_virtual_abs = 0.0
    has_logged_initial = False
    saved_break_bound = ''
    saved_break_value = 0.0
    saved_base_mom = 0.0
    saved_hedge_mom = 0.0
    saved_spread_mom = 0.0
    saved_start_i = 0
    entry_interval_long_arr[0] = entry_interval_long
    entry_interval_short_arr[0] = entry_interval_short
    current_max_sets_arr[0] = current_max_sets
    current_cash_arr[0] = current_cash
    for i in range(1, n):
        # Update PNL from previous to current
        delta_base = price_base_values[i] - price_base_values[i-1]
        delta_hedge = price_hedge_values[i] - price_hedge_values[i-1]
        pnl = current_base_pos * delta_base + current_hedge_pos * delta_hedge
        current_cash += pnl
        equity_per_set = base_size * price_base_values[i] + hedge_size * price_hedge_values[i]
        current_max_sets = min(glb_max_sets, int(current_cash * leverage / equity_per_set) if equity_per_set > 0 else 0)
        is_pre_zero = len(open_positions) == 0
        current_arbitrage_lower = arbitrage_lower_values[i]
        current_arbitrage_upper = arbitrage_upper_values[i]
        if current_arbitrage_lower != prev_lower or current_arbitrage_upper != prev_upper:
            if log_enabled:
                logs.append(timestamps[i] + ": 调整上下界 to lower=" + float_to_str(current_arbitrage_lower) + ", upper=" + float_to_str(current_arbitrage_upper) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos))
            bound_distance = current_arbitrage_upper - current_arbitrage_lower
            if bound_distance > 0:
                # Adjust dynamic factors based on profitable rates
                profitable_rate_long = profitable_closes_long / num_opens_long if num_opens_long > 0 else 0.0
                profitable_rate_short = profitable_closes_short / num_opens_short if num_opens_short > 0 else 0.0
                if num_opens_long >= min_ops:
                    if profitable_rate_long < widen_threshold:
                        dynamic_factor_long *= widen_step
                    elif profitable_rate_long > narrow_threshold:
                        dynamic_factor_long /= widen_step
                    dynamic_factor_long = max(min_dynamic, min(max_dynamic, dynamic_factor_long))
                if num_opens_short >= min_ops:
                    if profitable_rate_short < widen_threshold:
                        dynamic_factor_short *= widen_step
                    elif profitable_rate_short > narrow_threshold:
                        dynamic_factor_short /= widen_step
                    dynamic_factor_short = max(min_dynamic, min(max_dynamic, dynamic_factor_short))
                entry_interval = (bound_distance / 2) / current_max_sets if current_max_sets > 0 else 0.0
                entry_interval = max(entry_interval, min_entry_interval) # 应用最小加仓间隔
                entry_interval_long = entry_interval * dynamic_factor_long
                entry_interval_short = entry_interval * dynamic_factor_short
                center = (current_arbitrage_lower + current_arbitrage_upper) / 2
                center_long = center - exit_interval / 2
                center_short = center + exit_interval / 2
                long_grids_old = long_grids
                short_grids_old = short_grids
                long_grids = np.array([center_long - k * entry_interval_long for k in range(1, max_levels + 1)], dtype=np.float64)
                short_grids = np.array([center_short + k * entry_interval_short for k in range(1, max_levels + 1)], dtype=np.float64)
                if log_enabled:
                    logs.append(timestamps[i] + ": 调整网格 entry_interval=[long]" + float_to_str(entry_interval_long) + "[short]" + float_to_str(entry_interval_short) + ", center=" + float_to_str(center) + ", dynamic_long=" + float_to_str(dynamic_factor_long) + ", dynamic_short=" + float_to_str(dynamic_factor_short))
                # Adjust open positions to nearest new grid
                new_open = List()
                new_open.append((0.0, 0, 0.0))
                new_open.pop()
                for pos in open_positions:
                    entry_sp, dir_, entry_spread = pos
                    if dir_ == 1:
                        grids = long_grids
                        grids_old = long_grids_old
                    else:
                        grids = short_grids
                        grids_old = short_grids_old
                    if len(grids) > 0:
                        old_idx = np.where(grids_old == entry_sp)[0]
                        if len(old_idx) > 0:
                            old_idx = int(old_idx[0])
                            new_entry_sp = grids[old_idx]
                            new_open.append((new_entry_sp, dir_, entry_spread))
                            if log_enabled and new_entry_sp != entry_sp:
                                dir_str = "long" if dir_ == 1 else "short"
                                logs.append(timestamps[i] + ": 调整网格位置 " + dir_str + " from " + float_to_str(entry_sp) + " to " + float_to_str(new_entry_sp))
                        else:
                            new_open.append(pos)
                open_positions = new_open
            prev_lower = current_arbitrage_lower
            prev_upper = current_arbitrage_upper
        prev_sp = spread_values[i - 1]
        curr_sp = spread_values[i]
        # 风控：检测突破
        current_risk_lower = risk_lower_values[i]
        current_risk_upper = risk_upper_values[i]
        if not in_risk_mode:
            break_bound = ''
            if curr_sp > current_risk_upper:
                break_bound = 'upper'
                in_risk_mode = True
                break_direction = 1
                max_sp = curr_sp
                # 找起始点：最近上穿 center
                start_i = i - 1
                while start_i > 0:
                    if spread_values[start_i - 1] <= center and spread_values[start_i] > center:
                        break
                    start_i -= 1
            elif curr_sp < current_risk_lower:
                break_bound = 'lower'
                in_risk_mode = True
                break_direction = -1
                min_sp = curr_sp
                # 找起始点：最近下穿 center
                start_i = i - 1
                while start_i > 0:
                    if spread_values[start_i - 1] >= center and spread_values[start_i] < center:
                        break
                    start_i -= 1
            if in_risk_mode and start_i > 0:
                spread_mom = curr_sp - spread_values[start_i]
                base_mom = price_base_values[i] - price_base_values[start_i]
                hedge_mom = price_hedge_values[i] - price_hedge_values[start_i]
                # 判断亏钱腿
                base_sign = np.sign(base_mom)
                hedge_sign = np.sign(hedge_mom)
                spread_sign = np.sign(spread_mom)
                if base_sign == hedge_sign:
                    if spread_sign == base_sign:
                        losing_leg = 'base'
                    else:
                        losing_leg = 'hedge'
                else:
                    if abs(base_mom) >= abs(hedge_mom):
                        if spread_sign == base_sign:
                            losing_leg = 'base'
                        else:
                            losing_leg = 'hedge'
                    else:
                        if spread_sign == base_sign:
                            losing_leg = 'hedge'
                        else:
                            losing_leg = 'base'
                # 保存信息
                saved_break_bound = break_bound
                saved_break_value = current_risk_upper if break_direction == 1 else current_risk_lower
                saved_base_mom = base_mom
                saved_hedge_mom = hedge_mom
                saved_spread_mom = spread_mom
                saved_start_i = start_i
                # 准备减少持仓
                if losing_leg == 'base':
                    pre_pos = current_base_pos
                    size_leg = base_size
                    is_integer = base_is_integer
                    orders_leg = orders_base
                else:
                    pre_pos = current_hedge_pos
                    size_leg = hedge_size
                    is_integer = hedge_is_integer
                    orders_leg = orders_hedge
                virtual_full_abs = abs(pre_pos)
                max_virtual_abs = virtual_full_abs
                amount_to_reduce = reduce_ratio * max_virtual_abs
                reduced_direction = -np.sign(pre_pos) if pre_pos != 0 else 0
                if is_integer:
                    amount_to_reduce = np.floor(amount_to_reduce / size_leg) * size_leg
                reduced_amount = 0.0
                has_logged_initial = False
                if amount_to_reduce > 0 and reduced_direction != 0:
                    signed_reduce = reduced_direction * amount_to_reduce
                    label = get_label(pre_pos, signed_reduce)
                    orders_leg[i] += signed_reduce
                    if losing_leg == 'base':
                        risk_base_pos += signed_reduce
                        current_base_pos += signed_reduce
                    else:
                        risk_hedge_pos += signed_reduce
                        current_hedge_pos += signed_reduce
                    reduced_amount = amount_to_reduce
                    reduce_price = price_base_values[i] if losing_leg == 'base' else price_hedge_values[i]
                    dir_str = "多" if signed_reduce > 0 else "空"
                    judgment = "base_mom=" + float_to_str(base_mom) + " (sign " + float_to_str(base_sign) + "), hedge_mom=" + float_to_str(hedge_mom) + " (sign " + float_to_str(hedge_sign) + "), spread_mom=" + float_to_str(spread_mom) + " (sign " + float_to_str(spread_sign) + "), signs same: " + ("yes" if base_sign == hedge_sign else "no") + ", abs(base) >= abs(hedge): " + ("yes" if abs(base_mom) >= abs(hedge_mom) else "no") + ", so losing_leg=" + losing_leg
                    if log_enabled:
                        logs.append(timestamps[i] + ": 风控减仓 突破" + break_bound + "=" + float_to_str(saved_break_value) + ", spread=" + float_to_str(curr_sp) + ", judgment=" + judgment + ", " + label + " " + losing_leg + ", amount=" + float_to_str(signed_reduce) + " (" + dir_str + ")" + ", reduce_price=" + float_to_str(reduce_price) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos))
                    has_logged_initial = True
                reduced_leg = losing_leg
        # Check for closing open positions
        new_open = List()
        new_open.append((0.0, 0, 0.0))
        new_open.pop()
        for pos in open_positions:
            entry_sp, dir_, entry_spread = pos
            close = False
            if dir_ == 1:
                if curr_sp >= entry_sp + exit_interval:
                    close = True
            elif dir_ == -1:
                if curr_sp <= entry_sp - exit_interval:
                    close = True
            if close:
                pre_base = current_base_pos
                pre_hedge = current_hedge_pos
                order_base = -dir_ * base_size
                order_hedge = dir_ * hedge_size
                orders_base[i] += order_base
                arbitrage_base_pos += order_base
                current_base_pos += order_base
                orders_hedge[i] += order_hedge
                arbitrage_hedge_pos += order_hedge
                current_hedge_pos += order_hedge
                curr_lvl[i] += -dir_
                num_closes += 1
                pnl = curr_sp - entry_spread if dir_ == 1 else entry_spread - curr_sp
                if dir_ == 1:
                    num_closes_long += 1
                    if pnl > 0:
                        profitable_closes_long += 1
                else:
                    num_closes_short += 1
                    if pnl > 0:
                        profitable_closes_short += 1
                label_base = get_label(pre_base, order_base)
                label_hedge = get_label(pre_hedge, order_hedge)
                dir_str = "long" if dir_ == 1 else "short"
                if log_enabled:
                    logs.append(timestamps[i] + ": " + label_base + " base entry_sp=" + float_to_str(entry_sp) + ", spread=" + float_to_str(curr_sp) + ", size=" + float_to_str(order_base) + "; " + label_hedge + " hedge, size=" + float_to_str(order_hedge) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos) + ", levels=" + int_to_str(len(open_positions)) + ", new_lvls=" + int_to_str(len(new_open)))
            else:
                new_open.append(pos)
        open_positions = new_open
        # 更新创新高/低 和 检查回弹
        triggered_pos_zero = False
        if in_risk_mode:
            if break_direction == 1:
                if curr_sp > max_sp:
                    max_sp = curr_sp
                is_now_zero = len(open_positions) == 0
                if curr_sp <= max_sp - rebound_amount or is_now_zero:
                    triggered_pos_zero = is_now_zero
                    in_risk_mode = False
                    # 补回
                    if reduced_leg != '':
                        signed_supplement = - reduced_direction * reduced_amount
                        pre_pos = current_base_pos if reduced_leg == 'base' else current_hedge_pos
                        label = get_label(pre_pos, signed_supplement)
                        dir_str = "多" if signed_supplement > 0 else "空"
                        supplement_price = price_base_values[i] if reduced_leg == 'base' else price_hedge_values[i]
                        extreme_sp = max_sp
                        trigger_sp = max_sp - rebound_amount
                        if reduced_leg == 'base':
                            orders_base[i] += signed_supplement
                            risk_base_pos += signed_supplement
                            current_base_pos += signed_supplement
                        else:
                            orders_hedge[i] += signed_supplement
                            risk_hedge_pos += signed_supplement
                            current_hedge_pos += signed_supplement
                        if log_enabled:
                            logs.append(timestamps[i] + ": 风控补回, " + label + " " + reduced_leg + ", amount=" + float_to_str(signed_supplement) + " (" + dir_str + ")" + ", supplement_price=" + float_to_str(supplement_price) + ", spread=" + float_to_str(curr_sp) + ", extreme_sp=" + float_to_str(extreme_sp) + ", trigger_sp=" + float_to_str(trigger_sp) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos))
                        reduced_leg = ''
                        reduced_amount = 0.0
                        reduced_direction = 0
            elif break_direction == -1:
                if curr_sp < min_sp:
                    min_sp = curr_sp
                is_now_zero = len(open_positions) == 0
                if curr_sp >= min_sp + rebound_amount or is_now_zero:
                    triggered_pos_zero = is_now_zero
                    in_risk_mode = False
                    if reduced_leg != '':
                        signed_supplement = - reduced_direction * reduced_amount
                        pre_pos = current_base_pos if reduced_leg == 'base' else current_hedge_pos
                        label = get_label(pre_pos, signed_supplement)
                        dir_str = "多" if signed_supplement > 0 else "空"
                        supplement_price = price_base_values[i] if reduced_leg == 'base' else price_hedge_values[i]
                        extreme_sp = min_sp
                        trigger_sp = min_sp + rebound_amount
                        if reduced_leg == 'base':
                            orders_base[i] += signed_supplement
                            risk_base_pos += signed_supplement
                            current_base_pos += signed_supplement
                        else:
                            orders_hedge[i] += signed_supplement
                            risk_hedge_pos += signed_supplement
                            current_hedge_pos += signed_supplement
                        if log_enabled:
                            logs.append(timestamps[i] + ": 风控补回, " + label + " " + reduced_leg + ", amount=" + float_to_str(signed_supplement) + " (" + dir_str + ")" + ", supplement_price=" + float_to_str(supplement_price) + ", spread=" + float_to_str(curr_sp) + ", extreme_sp=" + float_to_str(extreme_sp) + ", trigger_sp=" + float_to_str(trigger_sp) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos))
                        reduced_leg = ''
                        reduced_amount = 0.0
                        reduced_direction = 0
        # Determine if just flatted to zero this time slice
        current_zero = len(open_positions) == 0
        just_flatted = (not is_pre_zero) and (triggered_pos_zero or current_zero)
        if just_flatted:
            num_opens_long = 0
            num_opens_short = 0
            num_closes_long = 0
            num_closes_short = 0
            profitable_closes_long = 0
            profitable_closes_short = 0
            dynamic_factor_long = 1
            dynamic_factor_short = 1
        # 原网格逻辑：开仓（如果没有just flatted）
        if not just_flatted:
            for grid in long_grids:
                if prev_sp > grid and curr_sp <= grid:
                    already_open = False
                    for pos in open_positions:
                        entry_sp, _, _ = pos
                        if entry_sp == grid:
                            already_open = True
                            break
                    if not already_open:
                        if len(open_positions) < current_max_sets:
                            open_positions.append((grid, 1, curr_sp))
                            pre_base = current_base_pos
                            pre_hedge = current_hedge_pos
                            orders_base[i] += base_size
                            arbitrage_base_pos += base_size
                            current_base_pos += base_size
                            orders_hedge[i] += -hedge_size
                            arbitrage_hedge_pos += -hedge_size
                            current_hedge_pos += -hedge_size
                            curr_lvl[i] += 1
                            num_opens += 1
                            num_opens_long += 1
                            label_base = get_label(pre_base, base_size)
                            label_hedge = get_label(pre_hedge, -hedge_size)
                            if log_enabled:
                                logs.append(timestamps[i] + ": " + label_base + " base at grid=" + float_to_str(grid) + ", spread=" + float_to_str(curr_sp) + ", size=" + float_to_str(base_size) + "; " + label_hedge + " hedge, size=" + float_to_str(-hedge_size) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos) + ", levels=" + int_to_str(len(open_positions)))
            for grid in short_grids:
                if prev_sp < grid and curr_sp >= grid:
                    already_open = False
                    for pos in open_positions:
                        entry_sp, _, _ = pos
                        if entry_sp == grid:
                            already_open = True
                            break
                    if not already_open:
                        if len(open_positions) < current_max_sets:
                            open_positions.append((grid, -1, curr_sp))
                            pre_base = current_base_pos
                            pre_hedge = current_hedge_pos
                            orders_base[i] += -base_size
                            arbitrage_base_pos += -base_size
                            current_base_pos += -base_size
                            orders_hedge[i] += hedge_size
                            arbitrage_hedge_pos += hedge_size
                            current_hedge_pos += hedge_size
                            curr_lvl[i] += -1
                            num_opens += 1
                            num_opens_short += 1
                            label_base = get_label(pre_base, -base_size)
                            label_hedge = get_label(pre_hedge, hedge_size)
                            if log_enabled:
                                logs.append(timestamps[i] + ": " + label_base + " base at grid=" + float_to_str(grid) + ", spread=" + float_to_str(curr_sp) + ", size=" + float_to_str(-base_size) + "; " + label_hedge + " hedge, size=" + float_to_str(hedge_size) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos) + ", levels=" + int_to_str(len(open_positions)))
        # 风控追加减仓
        if in_risk_mode and not current_zero:
            if reduced_leg != '':
                if reduced_leg == 'base':
                    curr_leg_pos = current_base_pos
                    size_leg = base_size
                    is_integer = base_is_integer
                    orders_leg = orders_base
                else:
                    curr_leg_pos = current_hedge_pos
                    size_leg = hedge_size
                    is_integer = hedge_is_integer
                    orders_leg = orders_hedge
             
                current_abs = abs(curr_leg_pos)
                current_virtual_abs = current_abs + reduced_amount
                if current_virtual_abs > max_virtual_abs:
                    max_virtual_abs = current_virtual_abs
                target_reduced = reduce_ratio * max_virtual_abs
                additional = target_reduced - reduced_amount
                if additional > 0:
                    if is_integer:
                        additional = np.floor(additional / size_leg) * size_leg
                    if additional > 0:
                        signed_reduce = reduced_direction * additional
                        pre_pos = curr_leg_pos
                        label = get_label(pre_pos, signed_reduce)
                        dir_str = "多" if signed_reduce > 0 else "空"
                        orders_leg[i] += signed_reduce
                        if reduced_leg == 'base':
                            risk_base_pos += signed_reduce
                            current_base_pos += signed_reduce
                        else:
                            risk_hedge_pos += signed_reduce
                            current_hedge_pos += signed_reduce
                        reduced_amount += additional
                        reduce_price = price_base_values[i] if reduced_leg == 'base' else price_hedge_values[i]
                        if has_logged_initial:
                            if log_enabled:
                                logs.append(timestamps[i] + ": 风控追加减仓, " + label + " " + reduced_leg + ", amount=" + float_to_str(signed_reduce) + " (" + dir_str + ")" + ", reduce_price=" + float_to_str(reduce_price) + ", spread=" + float_to_str(curr_sp) + ", current_virtual_abs=" + float_to_str(current_virtual_abs) + ", max_virtual_abs=" + float_to_str(max_virtual_abs) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos))
                        else:
                            base_sign = np.sign(saved_base_mom)
                            hedge_sign = np.sign(saved_hedge_mom)
                            spread_sign = np.sign(saved_spread_mom)
                            judgment = "base_mom=" + float_to_str(saved_base_mom) + " (sign " + float_to_str(base_sign) + "), hedge_mom=" + float_to_str(saved_hedge_mom) + " (sign " + float_to_str(hedge_sign) + "), spread_mom=" + float_to_str(saved_spread_mom) + " (sign " + float_to_str(spread_sign) + "), signs same: " + ("yes" if base_sign == hedge_sign else "no") + ", abs(base) >= abs(hedge): " + ("yes" if abs(saved_base_mom) >= abs(saved_hedge_mom) else "no") + ", so losing_leg=" + reduced_leg
                            if log_enabled:
                                logs.append(timestamps[i] + ": 风控减仓 突破" + saved_break_bound + "=" + float_to_str(saved_break_value) + ", spread=" + float_to_str(curr_sp) + ", judgment=" + judgment + ", " + label + " " + reduced_leg + ", amount=" + float_to_str(signed_reduce) + " (" + dir_str + ")" + ", reduce_price=" + float_to_str(reduce_price) + ", arb_base=" + float_to_str(arbitrage_base_pos) + ", risk_base=" + float_to_str(risk_base_pos) + ", total_base=" + float_to_str(current_base_pos) + ", arb_hedge=" + float_to_str(arbitrage_hedge_pos) + ", risk_hedge=" + float_to_str(risk_hedge_pos) + ", total_hedge=" + float_to_str(current_hedge_pos))
                            has_logged_initial = True
        # Subtract fees if there were orders this step
        if orders_base[i] != 0 or orders_hedge[i] != 0:
            fees = (abs(orders_base[i]) * price_base_values[i] + abs(orders_hedge[i]) * price_hedge_values[i]) * fee
            current_cash -= fees
        # Update profitable rates at each step
        profitable_rate_long_arr[i] = profitable_closes_long / num_opens_long if num_opens_long > 0 else 0.0
        profitable_rate_short_arr[i] = profitable_closes_short / num_opens_short if num_opens_short > 0 else 0.0
        entry_interval_long_arr[i] = entry_interval_long
        entry_interval_short_arr[i] = entry_interval_short
        current_max_sets_arr[i] = current_max_sets
        current_cash_arr[i] = current_cash
        max_open = max(max_open, len(open_positions))
    return orders_base, orders_hedge, curr_lvl, num_opens, num_closes, max_open, logs, profitable_rate_long_arr, profitable_rate_short_arr, entry_interval_long_arr, entry_interval_short_arr, current_max_sets_arr, current_cash_arr
def run_simulation(exit_interval, arbitrage_N, min_entry_interval, risk_N):
    # Calculate initial_upper and initial_lower for arbitrage
    first_day = spread.index[0]
    end_first_arbitrage = first_day + timedelta(days=arbitrage_N)
    spread_first_arbitrage = spread.loc[first_day:end_first_arbitrage]
    initial_arbitrage_lower = np.quantile(spread_first_arbitrage.values, 0.01)
    initial_arbitrage_upper = np.quantile(spread_first_arbitrage.values, 0.99)
    # Compute interval lower and upper based on arbitrage_N
    intervals_per_day = minutes_per_day / R
    arbitrage_window_size = int(arbitrage_N * intervals_per_day)
    arbitrage_lower_interval = pd.Series(index=interval_min.index, dtype=float)
    arbitrage_upper_interval = pd.Series(index=interval_max.index, dtype=float)
    for i in range(len(arbitrage_lower_interval)):
        if i == 0:
            arbitrage_lower_interval.iloc[i] = initial_arbitrage_lower
            arbitrage_upper_interval.iloc[i] = initial_arbitrage_upper
        else:
            start = max(0, i - arbitrage_window_size)
            arbitrage_lower_interval.iloc[i] = interval_min.iloc[start:i].min()
            arbitrage_upper_interval.iloc[i] = interval_max.iloc[start:i].max()
    # Expand to minute level
    arbitrage_lower_series = arbitrage_lower_interval.reindex(spread.index, method='pad')
    arbitrage_upper_series = arbitrage_upper_interval.reindex(spread.index, method='pad')
    arbitrage_lower_values = arbitrage_lower_series.values.astype(np.float64)
    arbitrage_upper_values = arbitrage_upper_series.values.astype(np.float64)
    # Calculate initial_upper and initial_lower for risk
    end_first_risk = first_day + timedelta(days=risk_N)
    spread_first_risk = spread.loc[first_day:end_first_risk]
    initial_risk_lower = np.quantile(spread_first_risk.values, 0.01)
    initial_risk_upper = np.quantile(spread_first_risk.values, 0.99)
    # Compute interval lower and upper based on risk_N
    risk_window_size = int(risk_N * intervals_per_day)
    risk_lower_interval = pd.Series(index=interval_min.index, dtype=float)
    risk_upper_interval = pd.Series(index=interval_max.index, dtype=float)
    for i in range(len(risk_lower_interval)):
        if i == 0:
            risk_lower_interval.iloc[i] = initial_risk_lower
            risk_upper_interval.iloc[i] = initial_risk_upper
        else:
            start = max(0, i - risk_window_size)
            risk_lower_interval.iloc[i] = interval_min.iloc[start:i].min()
            risk_upper_interval.iloc[i] = interval_max.iloc[start:i].max()
    # Expand to minute level
    risk_lower_series = risk_lower_interval.reindex(spread.index, method='pad')
    risk_upper_series = risk_upper_interval.reindex(spread.index, method='pad')
    risk_lower_values = risk_lower_series.values.astype(np.float64)
    risk_upper_values = risk_upper_series.values.astype(np.float64)
    # Compute backtest start based on max N
    max_N = max(arbitrage_N, risk_N)
    end_first_max = first_day + timedelta(days=max_N)
    backtest_start = np.searchsorted(spread.index, np.datetime64(end_first_max), side='right')
    if backtest_start >= len(spread):
        raise ValueError("No data after the first max N days")
    # Slice for backtest
    spread_values_bt = spread_values[backtest_start:]
    price_base_values_bt = price_base_values[backtest_start:]
    price_hedge_values_bt = price_hedge_values[backtest_start:]
    arbitrage_lower_values_bt = arbitrage_lower_values[backtest_start:]
    arbitrage_upper_values_bt = arbitrage_upper_values[backtest_start:]
    risk_lower_values_bt = risk_lower_values[backtest_start:]
    risk_upper_values_bt = risk_upper_values[backtest_start:]
    spread_bt_index = spread.index[backtest_start:]
    price_bt = price.iloc[backtest_start:]
    # Timestamps for bt
    timestamps = List.empty_list(unicode_type)
    for ts in spread_bt_index.astype(str):
        timestamps.append(ts)
    # No multiplier, fixed sizes
    effective_base_size = base_size
    effective_hedge_size = hedge_size
    # Run the simulation with new params
    orders_base_np, orders_hedge_np, curr_lvl_np, num_opens, num_closes, max_hold, logs, profitable_rate_long_arr, profitable_rate_short_arr, entry_interval_long_arr, entry_interval_short_arr, current_max_sets_arr, current_cash_arr = simulate(spread_values_bt, price_base_values_bt, price_hedge_values_bt, exit_interval, effective_base_size, effective_hedge_size, arbitrage_lower_values_bt, arbitrage_upper_values_bt, risk_lower_values_bt, risk_upper_values_bt, min_entry_interval, reduce_ratio, rebound_amount, timestamps, False, base_is_integer, hedge_is_integer, init_cash, fee, leverage)
    # Create orders DataFrame from NumPy arrays
    orders = pd.DataFrame({base_prd: orders_base_np, hedge_prd: orders_hedge_np}, index=spread_bt_index)
    # Create combined portfolio for total stats with cash sharing
    portfolio = vbt.Portfolio.from_orders(
        price_bt,
        size=orders,
        size_type='Amount',
        init_cash=vbt_init_cash, # Shared initial cash
        freq='1T',
        fees=fee,
        cash_sharing=True
    )
    # Group all columns into one group for combined stats
    portfolio.group_by = np.array([0, 0])
    # Total stats
    stats = portfolio.stats()
    # Single leg portfolios
    base_portfolio = vbt.Portfolio.from_orders(
        price_bt[base_prd],
        size=orders[base_prd],
        size_type='Amount',
        init_cash=vbt_init_cash,
        freq='1T',
        fees=fee,
    )
    hedge_portfolio = vbt.Portfolio.from_orders(
        price_bt[hedge_prd],
        size=orders[hedge_prd],
        size_type='Amount',
        init_cash=vbt_init_cash,
        freq='1T',
        fees=fee,
    )
    base_stats = base_portfolio.stats()
    hedge_stats = hedge_portfolio.stats()
    num_days = (spread_bt_index[-1] - spread_bt_index[0]).days + 1
    avg_daily_trades = (num_opens + num_closes) / num_days if num_days > 0 else 0
    total_realized_pnl = portfolio.trades.pnl.sum()
    avg_profit_per_close = total_realized_pnl / num_closes if num_closes > 0 else 0
    avg_fees_per_cycle = stats['Total Fees Paid'] / num_closes if num_closes > 0 else 0
    max_capital_usage = portfolio.init_cash - portfolio.cash().min()
    return (exit_interval, arbitrage_N, min_entry_interval, risk_N, stats['Total Return [%]'], stats['Sharpe Ratio'], stats['Max Drawdown [%]'], avg_daily_trades, avg_profit_per_close, avg_fees_per_cycle, max_hold, max_capital_usage, base_stats['Total Return [%]'], base_stats['Sharpe Ratio'], hedge_stats['Total Return [%]'], hedge_stats['Sharpe Ratio'])
if __name__ == '__main__':
    # Define search ranges (adjust as needed)
    N_values = [60, 120, 180, 360] # N的搜索范围，可调整
    N_values = [120] # N的搜索范围，可调整
    risk_N_values = [180, 240, 300, 360] # risk_N的搜索范围，可调整
    risk_N_values = [180] # risk_N的搜索范围，可调整
    # Define exit_intervals as multiples of base_exit_interval
    multiples_exit = [1, 1.5, 2, 2.5, 3]
    exit_intervals = [m * base_exit_interval for m in multiples_exit]
    # Define min_entry_intervals as multiples of base_min_entry_interval
    multiples_entry = [0.5, 1, 1.5, 2]
    multiples_entry = [0.5]
    min_entry_intervals = [m * base_min_entry_interval for m in multiples_entry]
    # Print parameters before search
    print(f'base_min_entry_interval: {base_min_entry_interval}')
    print(f'min_entry_intervals: {min_entry_intervals}')
    print(f'base_exit_interval: {base_exit_interval}')
    print(f'exit_intervals: {exit_intervals}')
    # Compute and print avg_max_sets using average prices
    avg_base_price = price_base.mean()
    avg_hedge_price = price_hedge.mean()
    avg_equity = base_size * avg_base_price + hedge_size * avg_hedge_price
    avg_max_sets = min(glb_max_sets, int(init_cash * leverage / avg_equity) if avg_equity > 0 else 0)
    print(f'avg_max_sets (ignoring glb): {avg_max_sets}, avg_equity: {avg_equity}')
    combinations = list(itertools.product(exit_intervals, N_values, min_entry_intervals, risk_N_values))
    # Run in parallel
    cpu_cnt = int(multiprocessing.cpu_count() / 10)
    with multiprocessing.Pool(processes=cpu_cnt) as pool:
        results = pool.starmap(run_simulation, combinations)
    # Collect and sort results
    key_stat = 'sharpe_ratio'
    # key_stat = 'total_return'
    df_results = pd.DataFrame(results, columns=['exit_interval', 'arbitrage_N', 'min_entry_interval', 'risk_N', 'total_return', 'sharpe_ratio', 'max_drawdown', 'avg_daily_trades', 'avg_profit_per_close', 'avg_fees_per_cycle', 'max_hold', 'max_capital_usage', 'base_total_return', 'base_sharpe', 'hedge_total_return', 'hedge_sharpe'])
    # df_results['max_leg_sharpe'] = df_results[['base_sharpe', 'hedge_sharpe']].max(axis=1)
    df_results['max_stats'] = df_results[[key_stat]].max(axis=1)
    print("Parameter Search Results (sorted by Max Leg Sharpe Ratio descending):")
    print(df_results.sort_values('max_stats', ascending=False))
    # Optionally, run and plot the best parameters
    best_row = df_results.loc[df_results['max_stats'].idxmax()]
    best_exit = best_row['exit_interval']
    best_arbitrage_N = int(best_row['arbitrage_N'])
    best_min_entry_interval = best_row['min_entry_interval']
    best_risk_N = int(best_row['risk_N'])
    print(f"\nBest parameters: exit_interval={best_exit}, arbitrage_N={best_arbitrage_N}, min_entry_interval={best_min_entry_interval}, risk_N={best_risk_N}")
    # Re-run simulation with best params for plots (recompute lower/upper with best_arbitrage_N and best_risk_N)
    first_day = spread.index[0]
    end_first_arbitrage = first_day + timedelta(days=best_arbitrage_N)
    spread_first_arbitrage = spread.loc[first_day:end_first_arbitrage]
    initial_arbitrage_lower = np.quantile(spread_first_arbitrage.values, 0.01)
    initial_arbitrage_upper = np.quantile(spread_first_arbitrage.values, 0.99)
    # Compute interval lower and upper based on best_arbitrage_N
    intervals_per_day = minutes_per_day / R
    arbitrage_window_size = int(best_arbitrage_N * intervals_per_day)
    arbitrage_lower_interval = pd.Series(index=interval_min.index, dtype=float)
    arbitrage_upper_interval = pd.Series(index=interval_max.index, dtype=float)
    for i in range(len(arbitrage_lower_interval)):
        if i == 0:
            arbitrage_lower_interval.iloc[i] = initial_arbitrage_lower
            arbitrage_upper_interval.iloc[i] = initial_arbitrage_upper
        else:
            start = max(0, i - arbitrage_window_size)
            arbitrage_lower_interval.iloc[i] = interval_min.iloc[start:i].min()
            arbitrage_upper_interval.iloc[i] = interval_max.iloc[start:i].max()
    # Expand to minute level
    arbitrage_lower_series = arbitrage_lower_interval.reindex(spread.index, method='pad')
    arbitrage_upper_series = arbitrage_upper_interval.reindex(spread.index, method='pad')
    arbitrage_lower_values = arbitrage_lower_series.values.astype(np.float64)
    arbitrage_upper_values = arbitrage_upper_series.values.astype(np.float64)
    # For risk
    end_first_risk = first_day + timedelta(days=best_risk_N)
    spread_first_risk = spread.loc[first_day:end_first_risk]
    initial_risk_lower = np.quantile(spread_first_risk.values, 0.01)
    initial_risk_upper = np.quantile(spread_first_risk.values, 0.99)
    risk_window_size = int(best_risk_N * intervals_per_day)
    risk_lower_interval = pd.Series(index=interval_min.index, dtype=float)
    risk_upper_interval = pd.Series(index=interval_max.index, dtype=float)
    for i in range(len(risk_lower_interval)):
        if i == 0:
            risk_lower_interval.iloc[i] = initial_risk_lower
            risk_upper_interval.iloc[i] = initial_risk_upper
        else:
            start = max(0, i - risk_window_size)
            risk_lower_interval.iloc[i] = interval_min.iloc[start:i].min()
            risk_upper_interval.iloc[i] = interval_max.iloc[start:i].max()
    # Expand to minute level
    risk_lower_series = risk_lower_interval.reindex(spread.index, method='pad')
    risk_upper_series = risk_upper_interval.reindex(spread.index, method='pad')
    risk_lower_values = risk_lower_series.values.astype(np.float64)
    risk_upper_values = risk_upper_series.values.astype(np.float64)
    # Compute backtest start based on max N
    max_N = max(best_arbitrage_N, best_risk_N)
    end_first_max = first_day + timedelta(days=max_N)
    backtest_start = np.searchsorted(spread.index, np.datetime64(end_first_max), side='right')
    if backtest_start >= len(spread):
        raise ValueError("No data after the first max N days")
    print(f'first_day,{first_day},end_first_max,{end_first_max},arbitrage_init_lower,{initial_arbitrage_lower},arbitrage_init_upper,{initial_arbitrage_upper},risk_init_lower,{initial_risk_lower},risk_init_upper,{initial_risk_upper},arbitrage_window_size,{arbitrage_window_size},risk_window_size,{risk_window_size},intervals_per_day,{intervals_per_day},spreadshape,{spread.shape},arbitrage_lower_seriesshape,{arbitrage_lower_series.shape},backtest_start,{backtest_start}')
    # Slice for backtest
    spread_values_bt = spread_values[backtest_start:]
    price_base_values_bt = price_base_values[backtest_start:]
    price_hedge_values_bt = price_hedge_values[backtest_start:]
    arbitrage_lower_values_bt = arbitrage_lower_values[backtest_start:]
    arbitrage_upper_values_bt = arbitrage_upper_values[backtest_start:]
    risk_lower_values_bt = risk_lower_values[backtest_start:]
    risk_upper_values_bt = risk_upper_values[backtest_start:]
    spread_bt_index = spread.index[backtest_start:]
    price_bt = price.iloc[backtest_start:]
    # Timestamps for bt
    timestamps = List.empty_list(unicode_type)
    for ts in spread_bt_index.astype(str):
        timestamps.append(ts)
    orders_base_np, orders_hedge_np, curr_lvl_np, num_opens, num_closes, max_hold, logs, profitable_rate_long_arr, profitable_rate_short_arr, entry_interval_long_arr, entry_interval_short_arr, current_max_sets_arr, current_cash_arr = simulate(spread_values_bt, price_base_values_bt, price_hedge_values_bt, best_exit, base_size, hedge_size, arbitrage_lower_values_bt, arbitrage_upper_values_bt, risk_lower_values_bt, risk_upper_values_bt, best_min_entry_interval, reduce_ratio, rebound_amount, timestamps, True, base_is_integer, hedge_is_integer, init_cash, fee, leverage)
    orders = pd.DataFrame({base_prd: orders_base_np, hedge_prd: orders_hedge_np}, index=spread_bt_index)
    lvls = pd.DataFrame({'LVL': curr_lvl_np}, index=spread_bt_index)
    base_portfolio = vbt.Portfolio.from_orders(
        price_bt[base_prd],
        size=orders[base_prd],
        size_type='Amount',
        init_cash=vbt_init_cash,
        freq='1T',
        fees=fee
    )
    hedge_portfolio = vbt.Portfolio.from_orders(
        price_bt[hedge_prd],
        size=orders[hedge_prd],
        size_type='Amount',
        init_cash=vbt_init_cash,
        freq='1T',
        fees=fee
    )
    portfolio = vbt.Portfolio.from_orders(
        price_bt,
        size=orders,
        size_type='Amount',
        init_cash=vbt_init_cash,
        freq='1T',
        fees=fee,
        cash_sharing=True
    )
    portfolio.group_by = np.array([0, 0])
    total_stats = portfolio.stats()
    num_days = (spread_bt_index[-1] - spread_bt_index[0]).days + 1
    avg_daily_trades = (num_opens + num_closes) / num_days if num_days > 0 else 0
    total_realized_pnl = portfolio.trades.pnl.sum()
    avg_profit_per_close = total_realized_pnl / num_closes if num_closes > 0 else 0
    avg_fees_per_cycle = total_stats['Total Fees Paid'] / num_closes if num_closes > 0 else 0
    max_capital_usage = portfolio.init_cash - portfolio.cash().min()
    print("Best Parameters Total Trading Results Statistics:")
    print(total_stats)
    print(f"Average Daily Trades: {avg_daily_trades}")
    print(f"Average Profit Per Close: {avg_profit_per_close}")
    print(f"Average Fees Per Cycle: {avg_fees_per_cycle}")
    print(f"Maximum Hold: {max_hold}")
    print(f"Maximum Capital Usage: {max_capital_usage}")
    print(f"\n{base_prd} Trading Results Statistics:")
    print(base_portfolio.stats())
    print(f"\n{hedge_prd} Trading Results Statistics:")
    print(hedge_portfolio.stats())
    # Calculate per-minute returns, mean, variance, and Kelly f
    pos_base = orders[base_prd].cumsum()
    pos_hedge = orders[hedge_prd].cumsum()
    minute_returns = []
    minute_pnls = []
    for i in range(1, len(price_bt)):
        start_pos_b = pos_base.iloc[i-1]
        start_pos_h = pos_hedge.iloc[i-1]
        start_p_b = price_bt[base_prd].iloc[i-1]
        start_p_h = price_bt[hedge_prd].iloc[i-1]
        equity = abs(start_pos_b) * start_p_b + abs(start_pos_h) * start_p_h
        d_b = price_bt[base_prd].iloc[i] - start_p_b
        d_h = price_bt[hedge_prd].iloc[i] - start_p_h
        benefit = start_pos_b * d_b + start_pos_h * d_h
        minute_pnls.append(benefit)
        if equity > 0:
            ret = benefit / equity
        else:
            ret = 0.0
        minute_returns.append(ret)
    returns_array = np.array(minute_returns)
    mean_ret = np.mean(returns_array)
    var_ret = np.var(returns_array)
    kelly_f = mean_ret / var_ret if var_ret != 0 else 0
    print(f"Mean per minute return: {mean_ret}")
    print(f"Variance of per minute returns: {var_ret}")
    print(f"Kelly leverage f: {kelly_f}")
  
    pnls_array = np.array(minute_pnls)
    mean_pnl = np.mean(pnls_array)
    var_pnl = np.var(pnls_array)
    kelly_f_pnl = mean_pnl / var_pnl if var_pnl != 0 else 0
    print(f"Mean per minute return: {mean_pnl}")
    print(f"Variance of per minute returns: {var_pnl}")
    print(f"Kelly leverage f: {kelly_f_pnl}")
  
    # Plot total equity curve
    plt.figure(figsize=(10, 6))
    portfolio.value().plot()
    plt.title('Total Equity Curve (Best Params)')
    plt.xlabel('Date')
    plt.ylabel('Equity')
    plt.show()
    # Plot base_prd equity curve
    plt.figure(figsize=(10, 6))
    base_portfolio.value().plot()
    plt.title(f'{base_prd} Equity Curve (Best Params)')
    plt.xlabel('Date')
    plt.ylabel('Equity')
    plt.show()
    # Plot hedge_prd equity curve
    plt.figure(figsize=(10, 6))
    hedge_portfolio.value().plot()
    plt.title(f'{hedge_prd} Equity Curve (Best Params)')
    plt.xlabel('Date')
    plt.ylabel('Equity')
    plt.show()
    with open('simulation_log.txt', 'w') as f:
        for log in logs:
            f.write(log + '\n')
    # Plot spread with lower and upper series
    plt.figure(figsize=(10, 6))
    spread.plot(label='spread')
    arbitrage_lower_series.plot(label='arbitrage_lower_series')
    arbitrage_upper_series.plot(label='arbitrage_upper_series')
    risk_lower_series.plot(label='risk_lower_series')
    risk_upper_series.plot(label='risk_upper_series')
    plt.title('Spread with Arbitrage and Risk Bounds')
    plt.xlabel('Date')
    plt.ylabel('Value')
    plt.legend()
    plt.show()
    # Plot profit_rate_long with spread_bt
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt = pd.Series(spread_values_bt, index=spread_bt_index)
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    prof_long_series = pd.Series(profitable_rate_long_arr, index=spread_bt_index)
    prof_long_series.replace(0, np.nan).ffill().fillna(0).plot(ax=ax2, color='red', label='profit_rate_long')
    ax2.set_ylabel('profit_rate_long')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and profit_rate_long')
    plt.show()
    # Plot profit_rate_short with spread_bt
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    prof_short_series = pd.Series(profitable_rate_short_arr, index=spread_bt_index)
    prof_short_series.replace(0, np.nan).ffill().fillna(0).plot(ax=ax2, color='green', label='profit_rate_short')
    ax2.set_ylabel('profit_rate_short')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and profit_rate_short')
    plt.show()
    # Plot entry_interval_long with spread_bt
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    entry_long_series = pd.Series(entry_interval_long_arr, index=spread_bt_index).ffill()
    entry_long_series.plot(ax=ax2, color='blue', label='entry_interval_long')
    ax2.set_ylabel('entry_interval_long')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and entry_interval_long')
    plt.show()
    # Plot entry_interval_short with spread_bt
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    entry_short_series = pd.Series(entry_interval_short_arr, index=spread_bt_index).ffill()
    entry_short_series.plot(ax=ax2, color='orange', label='entry_interval_short')
    ax2.set_ylabel('entry_interval_short')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and entry_interval_short')
    plt.show()
    # Plot spread_bt with current_max_sets
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    max_sets_series = pd.Series(current_max_sets_arr, index=spread_bt_index).ffill()
    max_sets_series.plot(ax=ax2, color='purple', label='current_max_sets')
    ax2.set_ylabel('current_max_sets')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and current_max_sets')
    plt.show()
    # Plot spread_bt with current_cash
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    cash_series = pd.Series(current_cash_arr, index=spread_bt_index).ffill()
    cash_series.plot(ax=ax2, color='brown', label='current_cash')
    ax2.set_ylabel('current_cash')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and current_cash')
    plt.show()
    # Plot spread_bt with abs position
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    pos_base = pos_base / base_size
    abs_pos_series = pos_base.abs()
    abs_pos_series.plot(ax=ax2, color='black', label='abs_position')
    ax2.set_ylabel('abs_position')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and abs_position')
    plt.show()
    # Plot spread_bt with long position
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    long_pos_series = pos_base.clip(lower=0)
    long_pos_series.plot(ax=ax2, color='green', label='long_position')
    ax2.set_ylabel('long_position')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and long_position')
    plt.show()
    # Plot spread_bt with short position
    plt.figure(figsize=(10, 6))
    ax = plt.gca()
    spread_bt.plot(ax=ax, label='价差行情')
    ax.set_ylabel('价差行情')
    ax2 = ax.twinx()
    short_pos_series = -pos_base.clip(upper=0)
    short_pos_series.plot(ax=ax2, color='red', label='short_position')
    ax2.set_ylabel('short_position')
    ax.legend(loc='upper left')
    ax2.legend(loc='upper right')
    plt.title('价差行情 and short_position')
    plt.show()