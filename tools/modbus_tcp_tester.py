import inspect
import tkinter as tk
from tkinter import ttk, messagebox

from pymodbus.client import ModbusTcpClient


REG_STAT_MODE = 0
REG_STAT_ERROR = 1
REG_SET_TIME_SECONDS = 6
REG_COMMAND = 7
REG_REMAINING_TIME_SECONDS = 8
REG_POWER_STATE = 9
REG_DOOR_STATE = 10
REG_LIMIT_SWITCH_STATE = 11
REG_MOTOR_DIRECTION = 12
REG_PERMISSION_STATE = 13
REG_COMMAND_RESULT = 14
REG_OPEN_REMAINING_SECONDS = 15

IREG_TIME_ALL = 0
IREG_TIME_HEAT = 1
IREG_END_OPEN = 20
IREG_END_CLOSED = 21

STAT_INITIAL = 1
STAT_OPEN = 11
STAT_HEAT = 12
STAT_CLOSE = 13
STAT_COMPLETE = 19
STAT_ERROR = 99

CMD_OPEN_DOOR = 1
CMD_CLOSE_DOOR = 2
CMD_POWER_ON = 4
CMD_POWER_OFF = 5
CMD_CLEAR_ERROR = 6

COMMAND_RESULT_TEXT = {
    0: "OK",
    1: "UNKNOWN_COMMAND",
    2: "BUSY",
    3: "NOT_ALLOWED",
    4: "INVALID_VALUE",
    6: "MANUAL_CONTROL_FORBIDDEN",
}

POWER_STATE_TEXT = {
    0: "OFF",
    1: "ON_PRE_SHUNT",
    2: "ON_SHUNTED",
}

DOOR_STATE_TEXT = {
    0: "BETWEEN",
    1: "OPEN",
    2: "CLOSED",
    3: "OPENING",
    4: "CLOSING",
}

MOTOR_DIRECTION_TEXT = {
    0: "STOP",
    1: "OPEN",
    2: "CLOSE",
}

ERROR_TEXT = {
    0: "NONE",
    100: "MANUAL_MODE_REQUIRED",
    101: "COMMAND_BUSY",
    102: "INVALID_COMMAND_OR_STATE",
    105: "DIRECT_RELAY_CONTROL_FORBIDDEN",
}

STAT_MODE_TEXT = {
    1: "READY",
    11: "OPENING",
    12: "HEATING",
    13: "CLOSING",
    19: "COMPLETE",
    99: "ERROR",
}


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Modbus TCP tester - microwave")
        self.root.geometry("760x520")
        self.root.minsize(720, 460)

        self.client = None
        self.connected = False
        self.door_closed = False

        self.host_var = tk.StringVar(value="192.168.1.50")
        self.port_var = tk.StringVar(value="502")
        self.unit_var = tk.StringVar(value="1")
        self.time_var = tk.StringVar(value="30")
        self.status_var = tk.StringVar(value="Не подключено")
        self.values_var = tk.StringVar(value="-")

        self._build_ui()
        self._tick()

    def _build_ui(self):
        root = ttk.Frame(self.root, padding=10)
        root.grid(sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(3, weight=1)

        conn = ttk.LabelFrame(root, text="Подключение", padding=8)
        conn.grid(row=0, column=0, sticky="ew")

        ttk.Label(conn, text="IP:").grid(row=0, column=0, sticky="w")
        self.host_entry = ttk.Entry(conn, textvariable=self.host_var, width=16)
        self.host_entry.grid(row=0, column=1, sticky="w")
        ttk.Label(conn, text="Port:").grid(row=0, column=2, padx=(8, 0), sticky="w")
        self.port_entry = ttk.Entry(conn, textvariable=self.port_var, width=8)
        self.port_entry.grid(row=0, column=3, sticky="w")
        ttk.Label(conn, text="Unit ID:").grid(row=0, column=4, padx=(8, 0), sticky="w")
        self.unit_entry = ttk.Entry(conn, textvariable=self.unit_var, width=8)
        self.unit_entry.grid(row=0, column=5, sticky="w")
        self.connect_button = ttk.Button(conn, text="Подключить", command=self.connect)
        self.connect_button.grid(row=0, column=6, padx=(8, 0))
        self.disconnect_button = ttk.Button(conn, text="Отключить", command=self.disconnect, state="disabled")
        self.disconnect_button.grid(row=0, column=7, padx=(4, 0))

        control = ttk.LabelFrame(root, text="Управление", padding=8)
        control.grid(row=1, column=0, sticky="ew", pady=(8, 0))

        ttk.Label(control, text="Время, сек:").grid(row=0, column=0, sticky="w")
        ttk.Entry(control, textvariable=self.time_var, width=10).grid(row=0, column=1, sticky="w")
        self.write_time_button = ttk.Button(control, text="Записать время", command=self.write_time, state="disabled")
        self.write_time_button.grid(row=0, column=2, padx=6)

        self.power_on_button = ttk.Button(control, text="Включить", command=self.start_heating, state="disabled")
        self.power_on_button.grid(row=1, column=0, pady=8)
        self.power_off_button = ttk.Button(control, text="Выключить", command=lambda: self.send_command(CMD_POWER_OFF), state="disabled")
        self.power_off_button.grid(row=1, column=1, pady=8)
        self.reset_button = ttk.Button(control, text="Сброс ошибки", command=lambda: self.send_state(STAT_INITIAL), state="disabled")
        self.reset_button.grid(row=1, column=2, pady=8)

        self.open_button = ttk.Button(control, text="Открыть", command=lambda: self.send_state(STAT_OPEN), state="disabled")
        self.open_button.grid(row=2, column=0, pady=8)
        self.close_button = ttk.Button(control, text="Закрыть", command=lambda: self.send_state(STAT_CLOSE), state="disabled")
        self.close_button.grid(row=2, column=1, pady=8)

        stat = ttk.LabelFrame(root, text="Статус", padding=8)
        stat.grid(row=2, column=0, sticky="ew", pady=(8, 0))
        ttk.Label(stat, textvariable=self.status_var).grid(row=0, column=0, sticky="w")

        values = ttk.LabelFrame(root, text="Регистры", padding=8)
        values.grid(row=3, column=0, sticky="nsew", pady=(8, 0))
        values.columnconfigure(0, weight=1)
        values.rowconfigure(0, weight=1)
        ttk.Label(values, textvariable=self.values_var, justify="left", anchor="nw").grid(row=0, column=0, sticky="nsew")

    def connect(self):
        self.disconnect(show_status=False)
        try:
            self.client = ModbusTcpClient(self.host_var.get().strip(), port=int(self.port_var.get()), timeout=1)
            self.connected = bool(self.client.connect())
            if self.connected:
                self._set_connection_ui(True)
                self.status_var.set("Подключено")
                self.poll()
            else:
                self.disconnect(show_status=False)
                self.status_var.set("Ошибка подключения")
        except Exception as exc:
            self.disconnect(show_status=False)
            self.status_var.set(f"Ошибка: {exc}")

    def _set_connection_ui(self, connected: bool):
        field_state = "disabled" if connected else "normal"
        for field in (self.host_entry, self.port_entry, self.unit_entry):
            field.configure(state=field_state)
        self.connect_button.configure(state="disabled" if connected else "normal")
        self.disconnect_button.configure(state="normal" if connected else "disabled")

        if not connected:
            for button in (self.write_time_button, self.power_on_button, self.power_off_button, self.reset_button, self.open_button, self.close_button):
                button.configure(state="disabled")

    def disconnect(self, show_status=True):
        if self.client:
            try:
                self.client.close()
            except Exception:
                pass
        self.client = None
        self.connected = False
        self.door_closed = False
        self._set_connection_ui(False)
        self.values_var.set("-")
        if show_status:
            self.status_var.set("Не подключено")

    def _unit(self):
        return int(self.unit_var.get())

    def _call_with_unit(self, fn, *args, **kwargs):
        unit = self._unit()
        params = inspect.signature(fn).parameters
        if "slave" in params:
            kwargs["slave"] = unit
        elif "unit" in params:
            kwargs["unit"] = unit
        elif "device_id" in params:
            kwargs["device_id"] = unit
        return fn(*args, **kwargs)

    def write_register(self, address: int, value: int):
        if not self.connected or not self.client:
            raise RuntimeError("Нет подключения")
        rr = self._call_with_unit(self.client.write_register, address, int(value) & 0xFFFF)
        if rr.isError():
            raise RuntimeError(str(rr))

    def send_command(self, command: int):
        try:
            self.write_register(REG_COMMAND, command)
            self.poll()
        except Exception as exc:
            messagebox.showerror("Modbus TCP", str(exc))

    def send_state(self, state: int):
        try:
            if state != STAT_INITIAL:
                self.write_register(REG_STAT_MODE, STAT_INITIAL)
            self.write_register(REG_STAT_MODE, state)
            self.poll()
        except Exception as exc:
            messagebox.showerror("Modbus TCP", str(exc))

    def start_heating(self):
        if not self.door_closed:
            messagebox.showerror("Безопасность", "Нагрев запрещён: концевик закрытой двери не замкнут.")
            return
        try:
            if not self.write_time():
                return
            self.send_state(STAT_HEAT)
        except Exception as exc:
            messagebox.showerror("Modbus TCP", str(exc))

    def write_time(self):
        try:
            seconds = int(self.time_var.get())
            if seconds <= 0 or seconds > 5999:
                raise ValueError("Время должно быть от 1 до 5999 секунд")
            self.write_register(REG_SET_TIME_SECONDS, seconds)
            self.poll()
            return True
        except Exception as exc:
            messagebox.showerror("Modbus TCP", str(exc))
            return False

    def read_registers(self, method_name: str, address: int, count: int):
        read_fn = getattr(self.client, method_name)
        params = inspect.signature(read_fn).parameters
        if "count" in params:
            return self._call_with_unit(read_fn, address, count=count)
        return self._call_with_unit(read_fn, address, count)

    @staticmethod
    def _permissions_text(bits: int) -> str:
        names = [
            ("power_on", 0),
            ("open", 1),
            ("close", 2),
            ("set_time", 4),
            ("power_off", 5),
        ]
        return ", ".join(f"{name}={1 if bits & (1 << bit) else 0}" for name, bit in names)

    def poll(self):
        if not self.connected or not self.client:
            return

        holding = self.read_registers("read_holding_registers", 0, 16)
        inputs = self.read_registers("read_input_registers", 0, 22)
        if holding.isError() or inputs.isError():
            self.status_var.set(f"Ошибка чтения: {holding if holding.isError() else inputs}")
            return

        r = holding.registers
        ir = inputs.registers
        stat_mode = r[REG_STAT_MODE]
        stat_error = r[REG_STAT_ERROR]
        power_state = r[REG_POWER_STATE]
        door_state = r[REG_DOOR_STATE]
        limit_state = r[REG_LIMIT_SWITCH_STATE]
        motor_direction = r[REG_MOTOR_DIRECTION]
        permissions = r[REG_PERMISSION_STATE]
        command_result = r[REG_COMMAND_RESULT]
        self.door_closed = bool(ir[IREG_END_CLOSED])

        self.power_on_button.configure(state="normal" if permissions & (1 << 0) and self.door_closed else "disabled")
        self.open_button.configure(state="normal" if permissions & (1 << 1) else "disabled")
        self.close_button.configure(state="normal" if permissions & (1 << 2) else "disabled")
        self.write_time_button.configure(state="normal" if permissions & (1 << 4) else "disabled")
        self.power_off_button.configure(state="normal" if permissions & (1 << 5) else "disabled")
        self.reset_button.configure(state="disabled" if door_state in (3, 4) else "normal")

        lines = [
            f"STAT_MODE={stat_mode} ({STAT_MODE_TEXT.get(stat_mode, 'UNKNOWN')})  "
            f"STAT_ERROR={stat_error} ({ERROR_TEXT.get(stat_error, 'UNKNOWN')})",
            f"SET_TIME_SECONDS={r[REG_SET_TIME_SECONDS]}  REMAINING_TIME_SECONDS={r[REG_REMAINING_TIME_SECONDS]}",
            f"POWER_STATE={power_state} ({POWER_STATE_TEXT.get(power_state, 'UNKNOWN')})",
            f"DOOR_STATE={door_state} ({DOOR_STATE_TEXT.get(door_state, 'UNKNOWN')})",
            f"LIMIT_SWITCH_STATE=0x{limit_state:04X}  virtual_open={1 if limit_state & 1 else 0}  DI5_closed={1 if limit_state & 2 else 0}",
            f"MOTOR_DIRECTION={motor_direction} ({MOTOR_DIRECTION_TEXT.get(motor_direction, 'UNKNOWN')})",
            f"OPEN_REMAINING_SECONDS={r[REG_OPEN_REMAINING_SECONDS]}",
            f"PERMISSION_STATE=0x{permissions:04X} [{self._permissions_text(permissions)}]",
            f"COMMAND_RESULT={command_result} ({COMMAND_RESULT_TEXT.get(command_result, 'UNKNOWN')})",
            f"TLM_TIME_ALL={ir[IREG_TIME_ALL]} h  TLM_TIME_HEAT={ir[IREG_TIME_HEAT]} h",
            f"TLM_END_OPEN={ir[IREG_END_OPEN]}  TLM_END_CLOSED={ir[IREG_END_CLOSED]}",
        ]
        self.values_var.set("\n".join(lines))
        self.status_var.set("Подключено")

    def _tick(self):
        try:
            self.poll()
        except Exception as exc:
            self.disconnect(show_status=False)
            self.status_var.set(f"Соединение потеряно: {exc}")
            self.values_var.set("-")
        self.root.after(500, self._tick)


if __name__ == "__main__":
    window = tk.Tk()
    App(window)
    window.mainloop()
