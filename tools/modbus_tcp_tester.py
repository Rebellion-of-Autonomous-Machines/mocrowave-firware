import inspect
import tkinter as tk
from tkinter import ttk, messagebox

from pymodbus.client import ModbusTcpClient


REG_COMMAND = 0
REG_SET_TIME_SECONDS = 1
REG_REMAINING_TIME_SECONDS = 2
REG_POWER_STATE = 3
REG_DOOR_STATE = 4
REG_LIMIT_SWITCH_STATE = 5
REG_MOTOR_DIRECTION = 6
REG_PERMISSION_STATE = 7
REG_COMMAND_RESULT = 8
REG_ERROR_CODE = 9
REG_OPEN_REMAINING_SECONDS = 10

CMD_OPEN_DOOR = 1
CMD_CLOSE_DOOR = 2
CMD_STOP_DOOR = 3
CMD_POWER_ON = 4
CMD_POWER_OFF = 5
CMD_CLEAR_ERROR = 6

COMMAND_RESULT_TEXT = {
    0: "OK",
    1: "UNKNOWN_COMMAND",
    2: "BUSY",
    3: "NOT_ALLOWED",
    4: "INVALID_VALUE",
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
    1: "STARTUP_CLOSE_TIMEOUT",
    2: "POWER_ON_DOOR_NOT_CLOSED",
    3: "POWER_ON_ZERO_TIME",
    4: "DOOR_COMMAND_WHILE_POWER_ON",
}


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("Modbus TCP tester - microwave")
        self.root.geometry("760x520")
        self.root.minsize(720, 460)

        self.client = None
        self.connected = False

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
        ttk.Entry(conn, textvariable=self.host_var, width=16).grid(row=0, column=1, sticky="w")
        ttk.Label(conn, text="Port:").grid(row=0, column=2, padx=(8, 0), sticky="w")
        ttk.Entry(conn, textvariable=self.port_var, width=8).grid(row=0, column=3, sticky="w")
        ttk.Label(conn, text="Unit ID:").grid(row=0, column=4, padx=(8, 0), sticky="w")
        ttk.Entry(conn, textvariable=self.unit_var, width=8).grid(row=0, column=5, sticky="w")
        ttk.Button(conn, text="Подключить", command=self.connect).grid(row=0, column=6, padx=(8, 0))
        ttk.Button(conn, text="Отключить", command=self.disconnect).grid(row=0, column=7, padx=(4, 0))

        control = ttk.LabelFrame(root, text="Управление", padding=8)
        control.grid(row=1, column=0, sticky="ew", pady=(8, 0))

        ttk.Label(control, text="Время, сек:").grid(row=0, column=0, sticky="w")
        ttk.Entry(control, textvariable=self.time_var, width=10).grid(row=0, column=1, sticky="w")
        ttk.Button(control, text="Записать время", command=self.write_time).grid(row=0, column=2, padx=6)

        ttk.Button(control, text="Включить", command=lambda: self.send_command(CMD_POWER_ON)).grid(row=1, column=0, pady=8)
        ttk.Button(control, text="Выключить", command=lambda: self.send_command(CMD_POWER_OFF)).grid(row=1, column=1, pady=8)
        ttk.Button(control, text="Сброс ошибки", command=lambda: self.send_command(CMD_CLEAR_ERROR)).grid(row=1, column=2, pady=8)

        ttk.Button(control, text="Открыть", command=lambda: self.send_command(CMD_OPEN_DOOR)).grid(row=2, column=0, pady=8)
        ttk.Button(control, text="Закрыть", command=lambda: self.send_command(CMD_CLOSE_DOOR)).grid(row=2, column=1, pady=8)
        ttk.Button(control, text="Стоп дверь", command=lambda: self.send_command(CMD_STOP_DOOR)).grid(row=2, column=2, pady=8)

        stat = ttk.LabelFrame(root, text="Статус", padding=8)
        stat.grid(row=2, column=0, sticky="ew", pady=(8, 0))
        ttk.Label(stat, textvariable=self.status_var).grid(row=0, column=0, sticky="w")

        values = ttk.LabelFrame(root, text="Регистры", padding=8)
        values.grid(row=3, column=0, sticky="nsew", pady=(8, 0))
        values.columnconfigure(0, weight=1)
        values.rowconfigure(0, weight=1)
        ttk.Label(values, textvariable=self.values_var, justify="left", anchor="nw").grid(row=0, column=0, sticky="nsew")

    def connect(self):
        self.disconnect()
        try:
            self.client = ModbusTcpClient(self.host_var.get().strip(), port=int(self.port_var.get()), timeout=1)
            self.connected = bool(self.client.connect())
            self.status_var.set("Подключено" if self.connected else "Ошибка подключения")
        except Exception as exc:
            self.connected = False
            self.status_var.set(f"Ошибка: {exc}")

    def disconnect(self):
        if self.client:
            try:
                self.client.close()
            except Exception:
                pass
        self.client = None
        self.connected = False

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

    def write_time(self):
        try:
            seconds = int(self.time_var.get())
            self.write_register(REG_SET_TIME_SECONDS, seconds)
            self.poll()
        except Exception as exc:
            messagebox.showerror("Modbus TCP", str(exc))

    @staticmethod
    def _permissions_text(bits: int) -> str:
        names = [
            ("power_on", 0),
            ("open", 1),
            ("close", 2),
            ("stop", 3),
            ("set_time", 4),
            ("power_off", 5),
        ]
        return ", ".join(f"{name}={1 if bits & (1 << bit) else 0}" for name, bit in names)

    def poll(self):
        if not self.connected or not self.client:
            return

        read_fn = self.client.read_holding_registers
        params = inspect.signature(read_fn).parameters
        kwargs = {}
        if "count" in params:
            kwargs["count"] = 11
            rr = self._call_with_unit(read_fn, 0, **kwargs)
        else:
            rr = self._call_with_unit(read_fn, 0, 11)

        if rr.isError():
            self.status_var.set(f"Ошибка чтения: {rr}")
            return

        r = rr.registers
        power_state = r[REG_POWER_STATE]
        door_state = r[REG_DOOR_STATE]
        limit_state = r[REG_LIMIT_SWITCH_STATE]
        motor_direction = r[REG_MOTOR_DIRECTION]
        permissions = r[REG_PERMISSION_STATE]
        command_result = r[REG_COMMAND_RESULT]
        error_code = r[REG_ERROR_CODE]

        self.time_var.set(str(r[REG_SET_TIME_SECONDS]))
        lines = [
            f"SET_TIME_SECONDS={r[REG_SET_TIME_SECONDS]}  REMAINING_TIME_SECONDS={r[REG_REMAINING_TIME_SECONDS]}",
            f"POWER_STATE={power_state} ({POWER_STATE_TEXT.get(power_state, 'UNKNOWN')})",
            f"DOOR_STATE={door_state} ({DOOR_STATE_TEXT.get(door_state, 'UNKNOWN')})",
            f"LIMIT_SWITCH_STATE=0x{limit_state:04X}  DI5_closed={1 if limit_state & 1 else 0}",
            f"MOTOR_DIRECTION={motor_direction} ({MOTOR_DIRECTION_TEXT.get(motor_direction, 'UNKNOWN')})",
            f"OPEN_REMAINING_SECONDS={r[REG_OPEN_REMAINING_SECONDS]}",
            f"PERMISSION_STATE=0x{permissions:04X} [{self._permissions_text(permissions)}]",
            f"COMMAND_RESULT={command_result} ({COMMAND_RESULT_TEXT.get(command_result, 'UNKNOWN')})",
            f"ERROR_CODE={error_code} ({ERROR_TEXT.get(error_code, 'UNKNOWN')})",
        ]
        self.values_var.set("\n".join(lines))
        self.status_var.set("Подключено")

    def _tick(self):
        try:
            self.poll()
        except Exception as exc:
            self.status_var.set(f"Ошибка: {exc}")
        self.root.after(500, self._tick)


if __name__ == "__main__":
    window = tk.Tk()
    App(window)
    window.mainloop()
