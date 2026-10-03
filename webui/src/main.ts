import { exec } from 'kernelsu';
import './style.css';

/** 目标包配置文件，与 app/src/main/jni/Tool/Targets.h 中的 kConfigPath 保持一致 */
const TARGETS_FILE = '/data/adb/il2cpp_tool/targets.txt';
/** 配置文件所在目录，写入前确保存在 */
const TARGETS_DIR = '/data/adb/il2cpp_tool';
/** 文件不存在或为空时写入的默认注释头 */
const DEFAULT_COMMENT = '# One package name per line. Edited by the KernelSU WebUI.';
/** 注入诊断日志文件，由 JNI 层在注入过程中追加写入 */
const INJECT_LOG_FILE = '/data/adb/il2cpp_tool/inject.log';
/** 命令执行超时（毫秒）：管理器回调未触发时避免界面永久卡住 */
const EXEC_TIMEOUT_MS = 8000;
/** 包名格式：至少包含一个点，仅允许字母、数字、下划线和点 */
const PACKAGE_NAME_RE = /^[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)+$/;

/** ksu.exec 的结果结构，与 kernelsu 包的 ExecResults 结构一致 */
interface ExecResult {
  errno: number;
  stdout: string;
  stderr: string;
}

/** targets.txt 的解析结果 */
interface ParsedTargets {
  /** 原样保留的注释行（# 开头） */
  comments: string[];
  /** 目标包名，保持文件中的顺序且已去重 */
  targets: string[];
}

/** 当前文件的注释行与目标包列表 */
let comments: string[] = [];
let targets: string[] = [];
/** 写操作进行中标记，避免重复提交 */
let busy = false;

/**
 * 按 id 取页面元素
 * @param id 元素 id
 * @returns 对应的元素，缺失时抛出异常（页面结构错误）
 */
function byId<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) {
    throw new Error(`页面缺少 #${id} 元素`);
  }
  return node as T;
}

const listEl = byId<HTMLUListElement>('list');
const emptyEl = byId<HTMLDivElement>('empty');
const inputEl = byId<HTMLInputElement>('pkgInput');
const addEl = byId<HTMLButtonElement>('btnAdd');
const refreshEl = byId<HTMLButtonElement>('btnRefresh');
const toastEl = byId<HTMLDivElement>('toast');
const logBoxEl = byId<HTMLDivElement>('logBox');
const logEmptyEl = byId<HTMLDivElement>('logEmpty');
const logRefreshEl = byId<HTMLButtonElement>('btnLogRefresh');

let toastTimer = 0;

/**
 * 显示页内提示条
 * @param message 提示内容
 */
function toast(message: string): void {
  toastEl.textContent = message;
  toastEl.classList.add('show');
  window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => toastEl.classList.remove('show'), 1800);
}

/**
 * 判断页面是否运行在 KernelSU 管理器的 WebView 中
 * @returns 注入的 ksu 接口存在时返回 true
 */
function hasKsuBridge(): boolean {
  return typeof (globalThis as Record<string, unknown>).ksu !== 'undefined';
}

/**
 * 执行 shell 命令，失败或超时统一返回错误结果而不抛异常
 * @param command 要执行的命令
 * @returns 命令的 errno/stdout/stderr
 */
async function runShell(command: string): Promise<ExecResult> {
  const timeout = new Promise<ExecResult>((resolve) => {
    window.setTimeout(() => resolve({ errno: -1, stdout: '', stderr: '执行超时' }), EXEC_TIMEOUT_MS);
  });
  try {
    return await Promise.race([exec(command), timeout]);
  } catch (error) {
    return { errno: -1, stdout: '', stderr: String(error) };
  }
}

/**
 * UTF-8 字符串转 base64，避免包名/注释中的字符被 shell 解析
 * @param text 原始文本
 * @returns base64 字符串
 */
function toBase64(text: string): string {
  const bytes = new TextEncoder().encode(text);
  let binary = '';
  for (const byte of bytes) {
    binary += String.fromCharCode(byte);
  }
  return btoa(binary);
}

/**
 * base64 转 UTF-8 字符串，自动忽略换行等空白字符
 * @param text base64 编码文本
 * @returns 解码后的文本，输入非法时返回空字符串（不抛异常）
 */
function fromBase64(text: string): string {
  try {
    const clean = text.replace(/\s+/g, '');
    if (clean === '') {
      return '';
    }
    const binary = atob(clean);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i += 1) {
      bytes[i] = binary.charCodeAt(i);
    }
    return new TextDecoder().decode(bytes);
  } catch {
    return '';
  }
}

/**
 * 解析文件内容：空行忽略，# 开头为注释，其余为包名并按出现顺序去重
 * @param text 文件内容
 * @returns 注释行与目标包名
 */
function parseTargets(text: string): ParsedTargets {
  const parsed: ParsedTargets = { comments: [], targets: [] };
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (line === '') {
      continue;
    }
    if (line.startsWith('#')) {
      parsed.comments.push(line);
      continue;
    }
    if (!parsed.targets.includes(line)) {
      parsed.targets.push(line);
    }
  }
  return parsed;
}

/**
 * 生成待写入的文件内容：先写注释行，再写目标包名
 * @returns 以换行结尾的文件内容
 */
function serializeTargets(): string {
  const lines = comments.length > 0 ? comments.slice() : [DEFAULT_COMMENT];
  return `${lines.concat(targets).join('\n')}\n`;
}

/** 渲染目标包列表 */
function render(): void {
  listEl.textContent = '';
  for (const pkg of targets) {
    const row = document.createElement('li');

    const label = document.createElement('span');
    label.className = 'pkg';
    label.textContent = pkg;

    const remove = document.createElement('button');
    remove.className = 'danger';
    remove.textContent = '\u00d7';
    remove.title = '删除';
    remove.addEventListener('click', () => {
      if (busy) {
        return;
      }
      targets = targets.filter((p) => p !== pkg);
      render();
      void save();
    });

    row.append(label, remove);
    listEl.appendChild(row);
  }
  emptyEl.style.display = targets.length > 0 ? 'none' : 'block';
}

/** 从设备读取目标包列表并渲染 */
async function load(): Promise<void> {
  const result = await runShell(`cat ${TARGETS_FILE} 2>/dev/null`);
  if (result.errno !== 0) {
    // 文件不存在时视为空列表，首次写入会自动创建
    comments = [];
    targets = [];
  } else {
    const parsed = parseTargets(result.stdout);
    comments = parsed.comments;
    targets = parsed.targets;
  }
  render();
}

/**
 * 按行渲染注入日志，并切换日志区与空提示的显隐
 * @param text 日志文件内容
 */
function renderLog(text: string): void {
  logBoxEl.textContent = '';
  const lines = text.split('\n');
  // 去掉末尾空行，避免文件结尾换行产生空白行
  while (lines.length > 0 && lines[lines.length - 1] === '') {
    lines.pop();
  }
  for (const line of lines) {
    const row = document.createElement('div');
    row.className = 'logline';
    // 空行用空格占位，保证高度不被折叠
    row.textContent = line === '' ? ' ' : line;
    logBoxEl.appendChild(row);
  }
  const isEmpty = lines.length === 0;
  logBoxEl.style.display = isEmpty ? 'none' : 'block';
  logEmptyEl.style.display = isEmpty ? 'block' : 'none';
}

/**
 * 展示日志区的提示文案（无日志/读取失败/非 KernelSU 环境）
 * @param message 提示内容
 */
function showLogMessage(message: string): void {
  logBoxEl.textContent = '';
  logBoxEl.style.display = 'none';
  logEmptyEl.textContent = message;
  logEmptyEl.style.display = 'block';
}

/** 从设备读取注入日志并渲染；任何失败都以提示收尾，不抛异常 */
async function loadLog(): Promise<void> {
  if (!hasKsuBridge()) {
    showLogMessage('需在 KernelSU WebUI 中打开');
    return;
  }
  // 用 base64 传输，规避日志中的特殊字符被 shell 解析
  const result = await runShell(`cat ${INJECT_LOG_FILE} 2>/dev/null | base64`);
  if (result.errno !== 0) {
    showLogMessage(`读取注入日志失败：${result.stderr || `错误码 ${result.errno}`}`);
    return;
  }
  const text = fromBase64(result.stdout);
  if (text.trim() === '') {
    // 文件不存在或内容为空
    showLogMessage('暂无注入日志');
    return;
  }
  renderLog(text);
}

/** 将当前列表写回设备 */
async function save(): Promise<void> {
  busy = true;
  addEl.disabled = true;
  // 用 base64 传输文件内容，规避特殊字符被 shell 解析
  const payload = toBase64(serializeTargets());
  const command =
    `mkdir -p ${TARGETS_DIR} && printf %s '${payload}' | base64 -d > ${TARGETS_FILE}` +
    ` && chmod 644 ${TARGETS_FILE}`;
  const result = await runShell(command);
  busy = false;
  addEl.disabled = false;
  if (result.errno === 0) {
    toast('已保存');
  } else {
    toast(`保存失败：${result.stderr || `错误码 ${result.errno}`}`);
  }
}

/** 添加输入框中的包名 */
function addTarget(): void {
  if (busy) {
    return;
  }
  const name = inputEl.value.trim();
  if (!PACKAGE_NAME_RE.test(name)) {
    toast('包名格式不正确');
    return;
  }
  if (targets.includes(name)) {
    toast('该包名已存在');
    inputEl.value = '';
    return;
  }
  targets.push(name);
  inputEl.value = '';
  render();
  void save();
}

addEl.addEventListener('click', addTarget);
inputEl.addEventListener('keydown', (event) => {
  if (event.key === 'Enter') {
    addTarget();
  }
});
refreshEl.addEventListener('click', () => {
  void load().then(() => toast('已刷新'));
});
logRefreshEl.addEventListener('click', () => {
  void loadLog().then(() => toast('已刷新'));
});

if (hasKsuBridge()) {
  void load();
  void loadLog();
} else {
  // 在普通浏览器中打开时给出提示，便于本地调试页面本身
  toast('请在 KernelSU 管理器内打开本页面');
  render();
  // 日志区同样提示需要 KernelSU 环境
  void loadLog();
}