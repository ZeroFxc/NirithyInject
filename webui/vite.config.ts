import { copyFileSync, existsSync, mkdirSync, readdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig, type Plugin } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

const projectDir = dirname(fileURLToPath(import.meta.url));

// 模块内的回退目录：build.gradle 在缺少 webui/dist 时会直接打包这里的产物
const moduleWebroot = resolve(projectDir, '../zygisk-module/webroot');

/**
 * 构建结束后把 dist 同步到模块回退目录，保证仓库里的回退页面始终是同一份产物。
 * 使用 closeBundle 以确保晚于 vite-plugin-singlefile 的内联处理。
 */
function syncToModuleWebroot(): Plugin {
  return {
    name: 'sync-to-module-webroot',
    closeBundle() {
      const distDir = resolve(projectDir, 'dist');
      if (!existsSync(distDir)) {
        return;
      }
      mkdirSync(moduleWebroot, { recursive: true });
      for (const name of readdirSync(distDir)) {
        copyFileSync(join(distDir, name), join(moduleWebroot, name));
      }
    },
  };
}

export default defineConfig({
  // WebView 从模块目录加载页面，资源必须使用相对路径
  base: './',
  plugins: [viteSingleFile(), syncToModuleWebroot()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    // 兼容旧版 Android WebView
    target: 'es2017',
  },
});