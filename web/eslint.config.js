import js from '@eslint/js'
import tseslint from 'typescript-eslint'
import vue from 'eslint-plugin-vue'

export default tseslint.config(
  { ignores: ['dist', 'node_modules'] },
  js.configs.recommended,
  tseslint.configs.recommended,
  vue.configs['flat/essential'],
  { files: ['**/*.vue'], languageOptions: { parserOptions: { parser: tseslint.parser } } },
  { rules: { '@typescript-eslint/no-explicit-any': 'off', 'no-undef': 'off', 'vue/multi-word-component-names': 'off' } }, // tsc already catches undefined globals
)
