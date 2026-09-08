import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'BusTub · Database Systems Atlas',
  description: '从 4KB 页到 MVCC 的数据库系统学习地图',
  lang: 'zh-CN',
  themeConfig: {
    siteTitle: 'BusTub / Atlas',
    nav: [
      { text: '总览', link: '/' },
      { text: '系统专题', link: '/system/' },
      { text: '存储专题', link: '/storage/' },
      { text: '实验报告', link: '/lab-notes/00-overview' },
      { text: '学习导航', link: '/guide/learning-path' },
      { text: 'STEP 索引', link: '/lab-notes/99-step-index' },
      { text: '源码仓库', link: 'https://github.com/cmu-db/bustub' }
    ],
    sidebar: {
      '/system/': [{ text: 'Database Systems', items: [
        { text: '系统总览', link: '/system/' },
        { text: '查询处理', link: '/system/query-processing' },
        { text: '事务与并发', link: '/system/transactions' },
        { text: '日志与恢复深入', link: '/system/recovery-deep' },
        { text: '现代存储引擎', link: '/system/modern-engines' },
        { text: '工程实践', link: '/system/engineering' }
      ]}],
      '/storage/': [
        { text: 'Storage Systems', items: [
          { text: '专题总览', link: '/storage/' },
          { text: 'Page & Tuple Layout', link: '/storage/page-layout' },
          { text: 'Buffer Pool 深入', link: '/storage/buffer-pool' },
          { text: 'WAL 与崩溃恢复', link: '/storage/recovery' },
          { text: 'B+ Tree vs LSM-Tree', link: '/storage/index-structures' },
          { text: 'Row Store vs Column Store', link: '/storage/row-column' },
          { text: '磁盘、SSD 与 I/O', link: '/storage/io' }
        ]}
      ],
      '/guide/': [
        { text: '学习导航', items: [
          { text: '学习路线', link: '/guide/learning-path' },
          { text: '源码导览', link: '/guide/source-tour' },
          { text: '术语词典', link: '/guide/glossary' }
        ]}
      ],
      '/lab-notes/': [
        { text: '从磁盘页开始', items: [
          { text: '实验总览', link: '/lab-notes/00-overview' },
          { text: 'P0 · 并发跳表', link: '/lab-notes/01-P0-skiplist' },
          { text: 'P1 · 缓冲池管理器', link: '/lab-notes/02-P1-buffer-pool' },
          { text: 'P2 · B+ 树索引', link: '/lab-notes/03-P2-index' },
          { text: 'P3 · 查询执行引擎', link: '/lab-notes/04-P3-execution' },
          { text: 'P4 · MVCC 并发控制', link: '/lab-notes/05-P4-concurrency' },
          { text: 'STEP 标记总索引', link: '/lab-notes/99-step-index' }
        ]}
      ]
    },
    search: { provider: 'local' },
    outline: [2, 3],
    footer: { message: 'CMU 15-445 · BusTub F2025 lab notes', copyright: 'Built for learning systems from first principles.' }
  }
})
